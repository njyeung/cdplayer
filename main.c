#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include <stdatomic.h>
#include <stdbool.h>

struct Track {
    int start_lba;
    int end_lba;
    int num_frames;
    int track_num;
};

typedef struct {
    int16_t *buf;
    size_t capacity;
    size_t mask;
    atomic_size_t write;
    atomic_size_t read;
} ringbuffer;

struct Player {
    ringbuffer rb;

    struct cdrom_read_audio *ra;
    int fd;
    
    snd_pcm_t* pcm;

    pthread_mutex_t track_mu;
    struct Track* current_track;
    int current_lba;

    pthread_t consumer_thread;
    pthread_t producer_thread;

    atomic_bool playing;
    atomic_bool quit;
};

const uint FRAME_SIZE = 2352;
const uint N_FRAMES = 25;
const uint RB_SIZE = 262144; // 2 ^ 18

// echos the number of samples written, or 0 if stopped or quit 
size_t player_write(struct Player *player, int16_t *samples, size_t num_samples) {
tryagain:

    bool quit = atomic_load(&player->quit);
    bool playing = atomic_load(&player->playing);
    if (quit || playing == false ) {
        return 0;
    }

    ringbuffer* rb = &player->rb;

    size_t w = atomic_load(&rb->write);
    size_t r = atomic_load(&rb->read);
    size_t free_space = rb->capacity - (w - r);

    if (num_samples > free_space) {
        usleep(1000);
        goto tryagain;
    }

    for (size_t i = 0; i < num_samples; i++) {
        // goofy math
        rb->buf[(w + i) & rb->mask] = samples[i];
    }

    atomic_store(&rb->write, w + num_samples);
    return num_samples;
}

// returns number of samples read into *samples
size_t player_read(struct Player *player, int16_t *samples, size_t num_samples) {
    ringbuffer* rb = &player->rb;

    size_t w = atomic_load(&rb->write);
    size_t r = atomic_load(&rb->read);
    size_t available = w - r;
    
    if (num_samples > available) {
        num_samples = available;
    }

    for (size_t i = 0; i < num_samples; i++) {
        samples[i] = rb->buf[(r + i) & rb->mask];
    }

    atomic_store(&rb->read, r + num_samples);
    return num_samples;
}

int play_samples(struct Player* player, int16_t* samples, size_t num_frames) {
    num_frames = num_frames/2; // alsa wants divided by 2 for stereo

    snd_pcm_sframes_t written = snd_pcm_writei(player->pcm, samples, num_frames);
    while (written < 0) {
        written = snd_pcm_recover(player->pcm, written, 1);
    } 

    return 0;
}

void* consumer(void* args) {
    struct Player* player = (struct Player *) args;
    
    const int BUF_SIZE = (N_FRAMES * FRAME_SIZE) / sizeof(int16_t);
    int16_t *buf = malloc(sizeof(int16_t) * BUF_SIZE);

    for(;;) {
        bool quit = atomic_load(&player->quit);
        if (quit) {
            break;
        }

        bool playing = atomic_load(&player->playing);
        if (playing == false) {
            usleep(1000);
            continue;
        }

        size_t num_read = player_read(player, buf, BUF_SIZE);
        if (num_read == 0) {
            usleep(1000);
            continue;
        } else {
            play_samples(player, buf,num_read);
        }
    }

    free(buf);
    
}

void* producer(void* args) {
    struct Player* player = (struct Player*) args;
    struct Track* prev_track = NULL;

    for (;;) {
        bool quit = atomic_load(&player->quit);
        if (quit) {
            break;
        }

        bool playing = atomic_load(&player->playing);
        if (playing == false) {
            usleep(1000);
            continue;
        }
        
        pthread_mutex_lock(&player->track_mu);

        // This is a messy solution since playing isn't mutated inside of the lock...
        if (player->current_track == NULL || player->current_lba == -1) {
            pthread_mutex_unlock(&player->track_mu);
            continue;
        }

        if(player->current_lba + N_FRAMES <= player->current_track->end_lba) {
            player->ra->addr.lba = player->current_lba;
            player->ra->nframes = N_FRAMES;

            ioctl(player->fd, CDROMREADAUDIO, player->ra);
            
            // cast to samples
            int16_t *samples = (int16_t *)player->ra->buf;
            int num_samples = (N_FRAMES * FRAME_SIZE) / sizeof(int16_t);

            player_write(player, samples, num_samples);

            player->current_lba += N_FRAMES;
        } else if (player->current_lba <= player->current_track->end_lba) { // play whatever frames are left
            int frames_left = player->current_track->num_frames % N_FRAMES;
            player->ra->addr.lba = player->current_track->end_lba - frames_left;
            player->ra->nframes = frames_left;
            ioctl(player->fd, CDROMREADAUDIO, player->ra);

            int16_t *samples = (int16_t *) player->ra->buf;
            int num_samples = (frames_left * FRAME_SIZE) / sizeof(int16_t);
            
            player_write(player, samples, num_samples);

            player->current_lba += frames_left;
        } else { // we reached the end, current_lba is >= current_track's end_lba
            atomic_store(&player->playing, false);
            player->current_track = NULL;
            player->current_lba = -1;
        } 
        
        pthread_mutex_unlock(&player->track_mu);
    }
}

// fetch header from CD
// caller has to free tracks
int _init_header(struct Track** tracks, size_t* tracks_len) {
    int fd = open("/dev/sr0", O_RDONLY | O_NONBLOCK);
    
    struct cdrom_tochdr hdr;
    ioctl(fd, CDROMREADTOCHDR, &hdr);

    const int FIRST_TRACK = hdr.cdth_trk0;
    const int LAST_TRACK = hdr.cdth_trk1;
    const size_t NUM_TRACKS = (LAST_TRACK - FIRST_TRACK + 1);
    
    *tracks_len = NUM_TRACKS;
    *tracks = malloc(NUM_TRACKS * sizeof(struct Track));

    struct Track *t = *tracks;

    int i = 0;
    for (int trackNum = hdr.cdth_trk0; trackNum <= hdr.cdth_trk1; trackNum++) {
        struct cdrom_tocentry entry;
        entry.cdte_track = trackNum;
        entry.cdte_format = CDROM_LBA;
        ioctl(fd, CDROMREADTOCENTRY, &entry);
        
        printf("=== TRACK %d ===\n", entry.cdte_track);
        printf("    Starting LBA: %d\n", entry.cdte_addr.lba);
        printf("    Starting MSF: %d\n", entry.cdte_addr.msf);
        printf("    CTRL: %d\n", entry.cdte_ctrl);
        printf("    Format: %d\n", entry.cdte_format);
        printf("    Data mode: %d\n", entry.cdte_datamode);

        t[i].start_lba = entry.cdte_addr.lba;
        t[i].track_num = trackNum;
        if (i > 0) {
            t[i-1].end_lba = entry.cdte_addr.lba; 
            t[i-1].num_frames = t[i-1].end_lba - t[i-1].start_lba;
        }
        i++;
    }

    // special case for the last entry, use CDROM_LEADOUT
    struct cdrom_tocentry entry;
    entry.cdte_track = CDROM_LEADOUT;
    entry.cdte_format = CDROM_LBA;
    ioctl(fd, CDROMREADTOCENTRY, &entry);
    t[NUM_TRACKS - 1].end_lba = entry.cdte_addr.lba;
    t[NUM_TRACKS - 1].num_frames = t[NUM_TRACKS - 1].end_lba - t[NUM_TRACKS - 1].start_lba;
    
    int err = close(fd);
    if (err != 0) {
        return 1;
    }

    return 0;
}

void _destroy_header(struct Track* tracks) {
    free(tracks);
}

int _init_player(struct Player** player) {
    *player = malloc(sizeof(struct Player));
    struct Player *p = *player;

    // ring buffer
    p->rb.buf = malloc(sizeof(int16_t) * RB_SIZE);
    p->rb.capacity = RB_SIZE;
    p->rb.mask = RB_SIZE - 1; // index % capacity is the same as index & mask

    p->fd = open("/dev/sr0", O_RDONLY | O_NONBLOCK);
    
    // tracks
    atomic_store(&p->playing, false); // false
    pthread_mutex_init(&p->track_mu, NULL);
    p->current_track = NULL;
    p->current_lba = -1;

    // read audio struct gets reused like my ex
    p->ra = malloc(sizeof(struct cdrom_read_audio));
    p->ra->buf = malloc(N_FRAMES * FRAME_SIZE);
    p->ra->addr_format = CDROM_LBA;
    p->ra->nframes = -1;
    p->ra->addr.lba = -1;

    // init pcm
    int err = snd_pcm_open(&p->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err != 0) {
        return 1;
    }
    // hard code rate to 44100 cuz its a CD wallahi
    err = snd_pcm_set_params(p->pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 44100, 1, 500000);
    if (err != 0) {
        snd_pcm_close(p->pcm);
        return 1;
    }

    atomic_store(&p->quit, false);
    // init consumer thread, bro exists just to consume the ring buffer
    pthread_create(&p->consumer_thread, NULL, consumer, p);

    // init producer thread
    pthread_create(&p->producer_thread, NULL, producer, p);

    return 0;
}

int _destroy_player(struct Player* p) {
    struct Player* player = p;
    
    // join thread
    atomic_store(&p->quit, true);
    pthread_join(p->consumer_thread, NULL);
    pthread_join(p->producer_thread, NULL);

    // drain and close pcm
    snd_pcm_drain(player->pcm);
    snd_pcm_close(player->pcm);

    // free ring buffer
    free(player->rb.buf);
    int err = close(player->fd);
    if (err != 0) {
        return 1;
    }
    free(player->ra->buf);
    free(player->ra);
    free(player);
    
    return 0;
}

int play(struct Track* track, struct Player* player) {

    pthread_mutex_lock(&player->track_mu);
    player->current_track = track;
    player->current_lba = track->start_lba;
    pthread_mutex_unlock(&player->track_mu);

    atomic_store(&player->playing, true);

    return 0;
}

int stop(struct Player* player) {

    atomic_store(&player->playing, false);

    pthread_mutex_lock(&player->track_mu);
    player->current_track = NULL;
    player->current_lba = -1;

    // advance read pointer of ring buffer past old data
    atomic_store(&player->rb.read, atomic_load(&player->rb.write));

    pthread_mutex_unlock(&player->track_mu);
}
int main() {
    int err;

    struct Track* tracks;
    size_t num_tracks;
    _init_header(&tracks, &num_tracks);

    struct Player* player; 
    _init_player(&player);

    err = play(&tracks[1], player);
    if (err != 0) {
        printf("PLAYER ERROR\n");
    }

    sleep(5);

    stop(player);

    printf("STOPPED PLAYER\n");

    sleep(5);

    printf("TRYING TO PLAY SECOND TRACK\n");
    err = play(&tracks[2], player);
    if (err != 0) {
        printf("PLAYER ERROR\n");
    }

    sleep(12);

    _destroy_player(player);
    _destroy_header(tracks);

    return 0;
}