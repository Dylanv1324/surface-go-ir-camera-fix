/*
 * ir-bridge: Capture IPU3 Y10 packed frames from CIO2 and output
 * 8-bit greyscale to a v4l2loopback device.
 *
 * IPU3 Y10 packed format: 25 pixels per 32-byte group.
 * Bytes 0-24: MSBs [9:2] of each pixel (high 8 bits)
 * Bytes 25-31: LSBs [1:0] packed 4 per byte
 *
 * For 8-bit output we take bytes 0-24 from each group.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define WIDTH 640
#define HEIGHT 480
#define NUM_BUFFERS 4

#define IPU3_Y10_PACKED_FMT v4l2_fourcc('i','p','3','y')

#define IPU3_GROUP_PIXELS 25
#define IPU3_GROUP_BYTES  32
#define IPU3_BPL ((((WIDTH) + IPU3_GROUP_PIXELS - 1) / IPU3_GROUP_PIXELS) * IPU3_GROUP_BYTES)

struct buffer {
    void *start;
    size_t length;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static void unpack_ipu3_y10(const unsigned char *src, unsigned char *dst,
                            int width, int height)
{
    int row, col, group;
    int groups_per_row = (width + IPU3_GROUP_PIXELS - 1) / IPU3_GROUP_PIXELS;

    for (row = 0; row < height; row++) {
        const unsigned char *row_src = src + row * IPU3_BPL;
        unsigned char *row_dst = dst + row * width;
        int pixels_done = 0;

        for (group = 0; group < groups_per_row; group++) {
            const unsigned char *g = row_src + group * IPU3_GROUP_BYTES;
            int pixels_in_group = width - pixels_done;
            if (pixels_in_group > IPU3_GROUP_PIXELS)
                pixels_in_group = IPU3_GROUP_PIXELS;

            for (col = 0; col < pixels_in_group; col++)
                row_dst[pixels_done + col] = g[col];

            pixels_done += pixels_in_group;
        }
    }
}

int main(int argc, char **argv)
{
    const char *src_dev = "/dev/video2";
    const char *dst_dev = "/dev/video20";
    int src_fd, dst_fd;
    struct buffer buffers[NUM_BUFFERS];
    unsigned char *grey;
    int frame_count = 0;

    if (argc >= 3) { src_dev = argv[1]; dst_dev = argv[2]; }

    grey = malloc(WIDTH * HEIGHT);
    if (!grey) { perror("malloc"); return 1; }

    src_fd = open(src_dev, O_RDWR);
    if (src_fd < 0) { perror("open src"); return 1; }

    struct v4l2_format sfmt = {0};
    sfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    sfmt.fmt.pix_mp.width = WIDTH;
    sfmt.fmt.pix_mp.height = HEIGHT;
    sfmt.fmt.pix_mp.pixelformat = IPU3_Y10_PACKED_FMT;
    sfmt.fmt.pix_mp.num_planes = 1;
    if (xioctl(src_fd, VIDIOC_S_FMT, &sfmt) < 0) {
        perror("VIDIOC_S_FMT src"); return 1;
    }
    fprintf(stderr, "ir-bridge: src format %ux%u, bytesperline=%u, sizeimage=%u\n",
            sfmt.fmt.pix_mp.width, sfmt.fmt.pix_mp.height,
            sfmt.fmt.pix_mp.plane_fmt[0].bytesperline,
            sfmt.fmt.pix_mp.plane_fmt[0].sizeimage);

    struct v4l2_requestbuffers req = {0};
    req.count = NUM_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(src_fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS"); return 1;
    }

    for (int i = 0; i < (int)req.count; i++) {
        struct v4l2_buffer buf = {0};
        struct v4l2_plane planes[1] = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;
        if (xioctl(src_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF"); return 1;
        }
        buffers[i].length = planes[0].length;
        buffers[i].start = mmap(NULL, planes[0].length,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                src_fd, planes[0].m.mem_offset);
        if (buffers[i].start == MAP_FAILED) {
            perror("mmap"); return 1;
        }
    }

    for (int i = 0; i < (int)req.count; i++) {
        struct v4l2_buffer buf = {0};
        struct v4l2_plane planes[1] = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;
        if (xioctl(src_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF"); return 1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(src_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON"); return 1;
    }

    dst_fd = open(dst_dev, O_WRONLY);
    if (dst_fd < 0) { perror("open dst"); return 1; }

    struct v4l2_format dfmt = {0};
    dfmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    dfmt.fmt.pix.width = WIDTH;
    dfmt.fmt.pix.height = HEIGHT;
    dfmt.fmt.pix.pixelformat = V4L2_PIX_FMT_GREY;
    dfmt.fmt.pix.sizeimage = WIDTH * HEIGHT;
    dfmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(dst_fd, VIDIOC_S_FMT, &dfmt) < 0) {
        perror("VIDIOC_S_FMT dst"); return 1;
    }

    fprintf(stderr, "ir-bridge: streaming %s -> %s (%dx%d, IPU3 BPL=%d)\n",
            src_dev, dst_dev, WIDTH, HEIGHT, IPU3_BPL);

    while (1) {
        struct v4l2_buffer buf = {0};
        struct v4l2_plane planes[1] = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = 1;

        if (xioctl(src_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            perror("VIDIOC_DQBUF"); break;
        }

        unpack_ipu3_y10(buffers[buf.index].start, grey, WIDTH, HEIGHT);

        if (write(dst_fd, grey, WIDTH * HEIGHT) < 0) {
            if (errno == EAGAIN) goto requeue;
            perror("write dst"); break;
        }

        frame_count++;
        if (frame_count % 100 == 0)
            fprintf(stderr, "ir-bridge: %d frames\n", frame_count);

requeue:
        if (xioctl(src_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF requeue"); break;
        }
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(src_fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < (int)req.count; i++)
        munmap(buffers[i].start, buffers[i].length);
    close(src_fd);
    close(dst_fd);
    free(grey);
    return 0;
}
