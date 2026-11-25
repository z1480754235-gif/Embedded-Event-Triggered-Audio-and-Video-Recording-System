#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define CAPTURE_BUFFER_COUNT	4
#define PREROLL_FRAME_COUNT	150

struct capture_buffer {
    void *start;
    size_t length;
};

static volatile sig_atomic_t save_request;

static void request_save(int signo)
{
    (void)signo;
    save_request = 1;
}

static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR &&
             !(request == VIDIOC_DQBUF && save_request));

    return ret;
}

static int save_preroll(const unsigned char *preroll, size_t frame_size,
			unsigned int oldest, unsigned int count)
{
    FILE *file;
	unsigned int i;

	file = fopen("/preroll-vga.yuyv", "wb");
	if (!file) {
		perror("fopen preroll");
		return -1;
	}

	for (i = 0; i < count; i++) {
		unsigned int index = (oldest + i) % PREROLL_FRAME_COUNT;
		const unsigned char *frame = preroll + (size_t)index * frame_size;

        /* 从frame地址读数据 每个元素占1个字节 写frame_size个元素 写到file文件流 */
		if (fwrite(frame, 1, frame_size, file) != frame_size) {
			perror("fwrite preroll");
			fclose(file);
			return -1;
		}
	}

	if (fclose(file) == EOF) {
		perror("fclose preroll");
		return -1;
	}

	return 0;
}

int main(void)
{
    struct v4l2_capability cap = { .capabilities = 0 };
    struct v4l2_format fmt = {0};
    struct v4l2_requestbuffers req = {0};
    struct sigaction action = { .sa_handler = request_save };
    struct capture_buffer buffers[CAPTURE_BUFFER_COUNT];
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    size_t frame_size;
    unsigned int write_index = 0;
    unsigned int mapped_count = 0;
    unsigned int valid_count = 0;
    unsigned int oldest;
    unsigned int i;
    unsigned int caps;
    unsigned char *preroll = NULL;
	int streaming = 0;
    int fd;
    int status = 1;

    fd = open("/dev/video0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/video0");
        return 1;
    }

    /* 请驱动把能力信息填进 cap */
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        goto out;
    }

    /* 检查 device_caps 字段是否有效 这里为有效 */
    caps = cap.capabilities;
    if (caps & V4L2_CAP_DEVICE_CAPS)
        caps = cap.device_caps;

    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device must support video capture and streaming\n");
        goto out;
    }

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 640;
    fmt.fmt.pix.height = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    /* 请求把 /dev/video0 的采集格式设为 640×480 YUYV */
    /* 调用前fmt里面是应用请求的格式，调用成功后里面是驱动实际采用的格式 */
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        goto out;
    }

    if (fmt.fmt.pix.width != 640 || fmt.fmt.pix.height != 480 ||
	    fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
		fprintf(stderr, "Driver returned a different format\n");
		goto out;
	}

    printf("Capture format: %ux%u YUYV\n",
	       fmt.fmt.pix.width, fmt.fmt.pix.height);
	printf("Bytes per line: %u, frame buffer size: %u bytes\n",
	       fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);

    frame_size = (size_t)fmt.fmt.pix.width * fmt.fmt.pix.height * 2;
    if (fmt.fmt.pix.bytesperline != fmt.fmt.pix.width * 2 ||
        fmt.fmt.pix.sizeimage != frame_size) {
        fprintf(stderr, "Expected tightly packed YUYV frames\n");
        goto out;
    }

    preroll = malloc(frame_size * PREROLL_FRAME_COUNT);
    if (!preroll) {
        perror("malloc preroll");
        goto out;
    }

    req.count = CAPTURE_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    /* 申请缓冲区 */
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        goto out;
    }

    if (req.count < 2 || req.count > CAPTURE_BUFFER_COUNT) {
		fprintf(stderr, "Unexpected buffer count: %u\n", req.count);
		goto out;
	}

    for (i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        void *start;

        buf.type = req.type;
        buf.memory = req.memory;
        buf.index = i;

        /* 查询单个缓冲区 */
        /* 驱动填回 buf.length 和 buf.m.offset */
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            goto out;
        }

        /* 返回应用用的虚拟地址 让应用和驱动访问同一块帧缓冲区 */
        start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, buf.m.offset);
        if (start == MAP_FAILED) {
            perror("mmap");
            goto out;
        }

        buffers[i].start = start;
        buffers[i].length = buf.length;
        mapped_count++;

        /* 把缓冲区交还给驱动 */
        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            goto out;
        }

        printf("Buffer %u queued, mapped size: %zu bytes\n",
		       i, buffers[i].length);
    }

    sigemptyset(&action.sa_mask); /* 不额外添加要阻塞的信号 */
    if (sigaction(SIGUSR1, &action, NULL) < 0) {
        perror("sigaction SIGUSR1");
        goto out;
    }

	/* 开始采集 */
	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		perror("VIDIOC_STREAMON");
		goto out;
	}
	streaming = 1;
	printf("Capturing video. To save preroll: kill -USR1 %ld\n", (long)getpid());
	fflush(stdout);

	while (!save_request) {
		struct v4l2_buffer buf = {0};

		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;

		/* 取出一帧给应用 */
		/* 驱动把缓冲区编号、有效字节数、帧序号和时间戳填进 buf */
		if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EINTR && save_request)
				break;
			perror("VIDIOC_DQBUF");
			goto out;
		}

		if (save_request)
			break;

		if (buf.index >= mapped_count ||
		    buf.bytesused == 0 ||
		    buf.bytesused > buffers[buf.index].length ||
		    (buf.flags & V4L2_BUF_FLAG_ERROR)) {
			fprintf(stderr, "Invalid frame: index=%u bytes=%u flags=0x%x\n",
				buf.index, buf.bytesused, buf.flags);
			goto out;
		}

        if (buf.bytesused != frame_size) {
			fprintf(stderr, "Unexpected frame size: %u\n", buf.bytesused);
			goto out;
		}

		memcpy(preroll + (size_t)write_index * frame_size,
		       buffers[buf.index].start, frame_size);

		write_index = (write_index + 1) % PREROLL_FRAME_COUNT;
		if (valid_count < PREROLL_FRAME_COUNT)
			valid_count++;

		/* 归还缓冲区给驱动 */
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("VIDIOC_QBUF");
			goto out;
		}
	}


	if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
		perror("VIDIOC_STREAMOFF");
		goto out;
	}
	streaming = 0;

	if (valid_count == 0) {
		fprintf(stderr, "Triggered before any frame was captured\n");
		goto out;
	}

	oldest = (write_index + PREROLL_FRAME_COUNT - valid_count) %
		 PREROLL_FRAME_COUNT;

	if (save_preroll(preroll, frame_size, oldest, valid_count) < 0)
		goto out;

	printf("Saved %u frames to /preroll-vga.yuyv\n", valid_count);

    status = 0;

out:
	if (streaming && xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
		perror("VIDIOC_STREAMOFF");
		status = 1;
	}

    while (mapped_count > 0) {
        mapped_count--;
        munmap(buffers[mapped_count].start, buffers[mapped_count].length);
    }

	free(preroll);

    close(fd);
    return status;
}
