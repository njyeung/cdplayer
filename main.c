#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <curl/curl.h>

#include "cJSON.h"

struct Track {
    int start_lba;
    int end_lba;
    int num_frames;
    int track_num;
    int length_us;

    char* title;
    char** artists;
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
    _Atomic(float) volume;

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

// it mutates the samples buffer btw
int play_samples(struct Player* player, int16_t* samples, size_t num_frames) {
    num_frames = num_frames/2; // alsa wants divided by 2 for stereo
    
    float vol = atomic_load(&player->volume);
    for (int16_t i = 0; i < num_frames * 2; i++) {
        samples[i] = (int16_t)(samples[i] * vol);
    }


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

            player->current_track = NULL;
            player->current_lba = -1;

            atomic_store(&player->playing, false);
        } 
        
        pthread_mutex_unlock(&player->track_mu);
    }
}

// I found this online breh
static const char hex[] = "0123456789ABCDEF";
static void write_hex(char *dst, unsigned int val, int digits) {
    for (int i = digits - 1; i >= 0; i--) {
        dst[i] = hex[val & 0xf];
        val >>= 4;
    }
}

// curl response buffer
struct mem_buf { char *data; size_t size; };
static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t real = size * nmemb;
    struct mem_buf *m = userdata;
    char *p = realloc(m->data, m->size + real + 1);
    if (!p) return 0;
    m->data = p;
    memcpy(m->data + m->size, ptr, real);
    m->size += real;
    m->data[m->size] = '\0';
    return real;
}
// fetch header from CD
// caller has to free tracks
int _init_header(struct Track** tracks, size_t* tracks_len) {
    int fd = open("/dev/sr0", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/sr0");
        return 1;
    }
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

        t[i].start_lba = entry.cdte_addr.lba;
        t[i].track_num = trackNum;
        if (i > 0) {
            t[i-1].end_lba = entry.cdte_addr.lba; 
            t[i-1].num_frames = t[i-1].end_lba - t[i-1].start_lba;
            t[i-1].length_us = ((uint64_t)t[i-1].num_frames * (uint64_t)1000000) / (uint64_t)75;
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
    t[NUM_TRACKS - 1].length_us = ((uint64_t)t[NUM_TRACKS - 1].num_frames * (uint64_t)1000000) / (uint64_t)75;

    int err = close(fd);
    if (err != 0) {
        return 1;
    }


    char string[2 + 2 + 100 * 8];
    memset(string, '0', sizeof(string));
    // first track number
    write_hex(string, t[0].track_num, 2);
    // last track number
    write_hex(string + 2, t[NUM_TRACKS - 1].track_num, 2);
    
    // slot 0 has lead out offset
    write_hex(string + 4, t[NUM_TRACKS - 1].end_lba + 150, 8);
    
    // slots 1-99 are filled with the offsets of each track
    for (int j = 0; j<NUM_TRACKS; j++) {
        write_hex(string + 4 + t[j].track_num * 8, t[j].start_lba + 150, 8);
    }

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(string, sizeof(string), hash);

    // EVP_EncodeBlock writes 28 chars + NUL for a 20-byte SHA1
    unsigned char base64enc[29];
    EVP_EncodeBlock(base64enc, hash, SHA_DIGEST_LENGTH);

    for (int j = 0; j < 28; j++) {
        switch (base64enc[j]) {
            case '+': base64enc[j] = '.'; break;
            case '/': base64enc[j] = '_'; break;
            case '=': base64enc[j] = '-'; break;
        }
    }

    char url[128];
    snprintf(url, sizeof(url), "https://musicbrainz.org/ws/2/discid/%s?fmt=json&inc=recordings+artists", base64enc);
    printf("%s\n", url);
    
    for (size_t k = 0; k < NUM_TRACKS; k++) {
        t[k].title = NULL;
        t[k].artists = NULL;
    }

    // fetch metadata from MusicBrainz
    struct mem_buf chunk = { .data = malloc(1), .size = 0 };
    chunk.data[0] = '\0';

    curl_global_init(CURL_GLOBAL_ALL);
    CURL *curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cdplayer/0.1 ( https://github.com/njyeung/cdplayer )");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_global_cleanup();

    if (res != CURLE_OK) {
        free(chunk.data);
        return 0;
    }

    cJSON *root = cJSON_Parse(chunk.data);
    free(chunk.data);

    cJSON *releases = cJSON_GetObjectItem(root, "releases");
    cJSON *release0 = cJSON_GetArrayItem(releases, 0);
    cJSON *media = cJSON_GetObjectItem(release0, "media");
    cJSON *media0 = cJSON_GetArrayItem(media, 0);
    cJSON *json_trks = cJSON_GetObjectItem(media0, "tracks");

    if (json_trks) {
        int n = cJSON_GetArraySize(json_trks);
        if ((size_t)n > NUM_TRACKS) n = NUM_TRACKS;

        for (int k = 0; k < n; k++) {

            cJSON *jt = cJSON_GetArrayItem(json_trks, k);
            cJSON *title = cJSON_GetObjectItem(jt, "title");
            
            if (title && cJSON_IsString(title)) {
                t[k].title = strdup(title->valuestring);
            }

            cJSON *credits = cJSON_GetObjectItem(jt, "artist-credit");
            int nc = cJSON_GetArraySize(credits);

            t[k].artists = malloc(sizeof(char *) * (nc + 1));

            for (int c = 0; c < nc; c++) {
                cJSON *ce = cJSON_GetArrayItem(credits, c);
                cJSON *nm = cJSON_GetObjectItem(ce, "name");
                t[k].artists[c] = (nm && cJSON_IsString(nm)) ? strdup(nm->valuestring) : strdup("");
            }
            t[k].artists[nc] = NULL;
        }
    }

    cJSON_Delete(root);

    for (size_t k = 0; k < NUM_TRACKS; k++) {
        printf("%s\n", t[k].title ? t[k].title : "(unknown)");
        printf("    Start LBA:   %d\n", t[k].start_lba);
        printf("    End LBA:     %d\n", t[k].end_lba);
        printf("    Num frames:  %d\n", t[k].num_frames);
        printf("    Length (us): %d\n", t[k].length_us);
        printf("    Artists:    ");
        if (t[k].artists) {
            for (char **a = t[k].artists; *a; a++) printf(" %s", *a);
        }
        printf("\n");
    }

    return 0;
}

void _destroy_header(struct Track* tracks, size_t tracks_len) {
    for (size_t i = 0; i < tracks_len; i++) {
        free(tracks[i].title);
        if (tracks[i].artists) {
            for (char **a = tracks[i].artists; *a; a++) free(*a);
            free(tracks[i].artists);
        }
    }
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
    atomic_store(&p->volume, 1.0);

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

void play(struct Player* player, struct Track* track) {

    atomic_store(&player->playing, true);

    pthread_mutex_lock(&player->track_mu);
    player->current_track = track;
    player->current_lba = track->start_lba;
    pthread_mutex_unlock(&player->track_mu);

}

void stop(struct Player* player) {

    atomic_store(&player->playing, false);

    pthread_mutex_lock(&player->track_mu);
    player->current_track = NULL;
    player->current_lba = -1;

    // advance read pointer of ring buffer past old data
    atomic_store(&player->rb.read, atomic_load(&player->rb.write));

    pthread_mutex_unlock(&player->track_mu);
    
}

// 1 = no disk in tray
// 2 = tray open
// 3 = reading tray
// 4 = disk in tray
static int tray_status() {
    int fd = open("/dev/sr0", O_RDONLY | O_NONBLOCK);
    int ret = ioctl(fd,0x5326);
    close(fd);
    return ret;
}


void set_volume(struct Player* player, float vol) {
    if(vol > 1.0 || vol < 0.0) {
        return;
    }
    
    atomic_store(&player->volume, vol);
}

int main() {
    for (;;) {
        while(tray_status() != 4) {
            sleep(1);
            // spin
        }

        struct Track* tracks;
        size_t num_tracks;
        _init_header(&tracks, &num_tracks);

        struct Player* player; 
        _init_player(&player);

        set_volume(player, 1);

        for (int i = 0; i<num_tracks; i++) {

            play(player, &tracks[i]);

            printf("PLAYING: %s\n", tracks[i].title);

            int status;
            while(atomic_load(&player->playing) && (status = tray_status()) == 4) {
                sleep(1);
            }
            if (status == 0) { // track ended, play the next song 
                stop(player);
            } else if (status == 1) { // cd player was opened
                stop(player);
                break;
            }
        }

        _destroy_player(player);
        _destroy_header(tracks, num_tracks);
    }
    

    return 0;
}
