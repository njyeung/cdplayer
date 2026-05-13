#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#include <stdlib.h>


struct Track {
    int start_lba;
    int end_lba;
    int num_frames;
    int track_num;
};

const int FRAME_SIZE = 2352;
const int N_FRAMES = 25;

int main() {
    int fd = open("/dev/sr0", O_RDONLY | O_NONBLOCK);
    
    struct cdrom_tochdr hdr;
    ioctl(fd, CDROMREADTOCHDR, &hdr);

    const int FIRST_TRACK = hdr.cdth_trk0;
    const int LAST_TRACK = hdr.cdth_trk1;
    const int NUM_TRACKS = (LAST_TRACK - FIRST_TRACK + 1);
    
    struct Track *tracks = malloc(NUM_TRACKS * sizeof(struct Track));

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

        tracks[i].start_lba = entry.cdte_addr.lba;
        tracks[i].track_num = trackNum;
        if (i > 0) {
            tracks[i-1].end_lba = entry.cdte_addr.lba; 
            tracks[i-1].num_frames = tracks[i-1].end_lba - tracks[i-1].start_lba;
        }
        i++;
    }

    // special case for the last entry
    struct cdrom_tocentry entry;
    entry.cdte_track = CDROM_LEADOUT;
    entry.cdte_format = CDROM_LBA;
    ioctl(fd, CDROMREADTOCENTRY, &entry);
    tracks[NUM_TRACKS - 1].end_lba = entry.cdte_addr.lba;
    tracks[NUM_TRACKS - 1].num_frames = tracks[NUM_TRACKS - 1].end_lba - tracks[NUM_TRACKS - 1].start_lba;
    
    for (int i = 0; i<NUM_TRACKS; i++) {
        printf("READING track: %d\n", tracks[i].track_num);
        
        // stuff we don't change
        struct cdrom_read_audio ra;
        ra.addr_format = CDROM_LBA;
        ra.buf = malloc(N_FRAMES * FRAME_SIZE);
        
        // stuff we change
        ra.addr.lba = -1;
        ra.nframes = -1;

        // step through the frames
        for (int j = tracks[i].start_lba; j+N_FRAMES < tracks[i].end_lba; j+=N_FRAMES) {
            // printf("ITERATING\n");
            ra.addr.lba = j;
            ra.nframes = N_FRAMES;
            ioctl(fd, CDROMREADAUDIO, &ra);
            
            // cast to samples
            int16_t *samples = (int16_t *)ra.buf;
            int16_t left_0  = samples[0];
            // print the first sample
            printf("%d\n", left_0);
        }

        // now print whatever frames are left
        int frames_left = tracks[i].num_frames % N_FRAMES;
        ra.addr.lba = tracks[i].end_lba - frames_left;
        ra.nframes = frames_left;
        ioctl(fd, CDROMREADAUDIO, &ra);
        for (int j = 0; j < frames_left * FRAME_SIZE; j++) {
            printf("%02x ", ra.buf[j]);
        }
        
        free(ra.buf);

        printf("\n\n");
    }

    int err = close(fd);
    printf("%d\n", err);
    printf("%d\n", fd);
    return 0;
}