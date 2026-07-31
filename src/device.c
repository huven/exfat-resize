/* SPDX-License-Identifier: MIT */

#define _FILE_OFFSET_BITS 64
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "device.h"

#include "block_device.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/disk.h>
#include <sys/ioctl.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#else
#error Unsupported operating system
#endif

static int block_device_read(
    void *context, uint64_t first_sector, uint32_t sector_count, void *buffer);
static int block_device_write(
    void *context, uint64_t first_sector, uint32_t sector_count, const void *buffer);
static int block_device_sync(void *context);

static void set_error(char *error, size_t size, const char *path)
{
	(void)snprintf(error, size, "%s: %s", path, strerror(errno));
}

static int is_in_use_error(int error_number)
{
	return error_number == EBUSY || error_number == EWOULDBLOCK || error_number == EAGAIN;
}

static void set_in_use_error(char *error, size_t size, const char *path)
{
	(void)snprintf(error, size, "%s: mounted or already in use", path);
}

static void record_io_error(struct device *device, const char *operation, int error_number)
{
	if (device->io_error_operation != NULL)
		return;
	device->io_error_operation = operation;
	device->io_error_number = error_number;
}

int device_open_flags(int is_block_device)
{
	int flags = O_RDWR;

#if defined(__APPLE__)
	(void)is_block_device;
	flags |= O_EXLOCK | O_NONBLOCK;
#else
	if (is_block_device)
		flags |= O_EXCL;
#endif
	return flags;
}

int device_open(struct device *device, const char *path, char *error, size_t error_size)
{
	struct stat st;
#if defined(__linux__)
	struct stat path_st;
#endif
	uint64_t bytes;
	uint32_t sector_size = 512;
	int flags;
	int fd;

#if defined(__linux__)
	if (stat(path, &path_st) != 0) {
		set_error(error, error_size, path);
		return -1;
	}
	if (!S_ISREG(path_st.st_mode) && !S_ISBLK(path_st.st_mode)) {
		(void)snprintf(error, error_size, "%s: not a regular file or block device", path);
		return -1;
	}
	flags = device_open_flags(S_ISBLK(path_st.st_mode));
#else
	flags = device_open_flags(0);
#endif
	fd = open(path, flags);

	if (fd < 0) {
		if (is_in_use_error(errno)) {
			set_in_use_error(error, error_size, path);
			return -1;
		}
		set_error(error, error_size, path);
		return -1;
	}
	if (fstat(fd, &st) != 0) {
		set_error(error, error_size, path);
		goto fail_after_open;
	}
#if defined(__linux__)
	if (st.st_dev != path_st.st_dev || st.st_ino != path_st.st_ino ||
	    S_ISREG(st.st_mode) != S_ISREG(path_st.st_mode) ||
	    S_ISBLK(st.st_mode) != S_ISBLK(path_st.st_mode)) {
		(void)snprintf(error, error_size, "%s: changed while opening", path);
		goto fail_after_open;
	}
#endif

	if (S_ISREG(st.st_mode)) {
#if defined(__linux__)
		if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
			if (is_in_use_error(errno))
				set_in_use_error(error, error_size, path);
			else
				set_error(error, error_size, path);
			goto fail_after_open;
		}
		/* The file size may have changed between the initial fstat() and flock(). */
		if (fstat(fd, &st) != 0) {
			set_error(error, error_size, path);
			goto fail_after_open;
		}
#endif
		bytes = (uint64_t)st.st_size;
	} else {
#if defined(__APPLE__)
		uint64_t blocks;
		if (ioctl(fd, DKIOCGETBLOCKSIZE, &sector_size) != 0 ||
		    ioctl(fd, DKIOCGETBLOCKCOUNT, &blocks) != 0) {
			set_error(error, error_size, path);
			goto fail_after_open;
		}
		if (sector_size > 0 && blocks > UINT64_MAX / sector_size) {
			(void)snprintf(error, error_size, "%s: device is too large", path);
			goto fail_after_open;
		}
		bytes = blocks * (uint64_t)sector_size;
#else
		int logical_size;
		if (ioctl(fd, BLKSSZGET, &logical_size) != 0 || logical_size <= 0 ||
		    ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
			set_error(error, error_size, path);
			goto fail_after_open;
		}
		sector_size = (uint32_t)logical_size;
#endif
	}

	if (!exfat_resize_sector_size_is_supported(sector_size)) {
		(void)snprintf(
		    error, error_size, "%s: unsupported logical sector size %" PRIu32, path, sector_size);
		goto fail_after_open;
	}
	if (bytes < sector_size) {
		(void)snprintf(error, error_size, "%s: device is too small", path);
		goto fail_after_open;
	}

	device->fd = fd;
	device->is_regular_file = S_ISREG(st.st_mode);
	device->io_error_operation = NULL;
	device->io_error_number = 0;
	device->block_device.context = device;
	device->block_device.sector_size = sector_size;
	device->block_device.sector_count = bytes / sector_size;
	device->block_device.read = block_device_read;
	device->block_device.write = block_device_write;
	device->block_device.sync = block_device_sync;
	return 0;

fail_after_open:
	(void)close(fd);
	return -1;
}

void device_close(struct device *device)
{
	if (device->fd >= 0)
		(void)close(device->fd);
	device->fd = -1;
}

static int transfer(
    struct device *device, void *buffer, uint64_t sector, uint32_t count, int write_data)
{
	uint64_t byte_offset;
	size_t total, done = 0;
	uint32_t sector_size = device->block_device.sector_size;
	uint64_t sector_count = device->block_device.sector_count;

	if ((uint64_t)count > sector_count || sector > sector_count - (uint64_t)count ||
	    count > SIZE_MAX / sector_size)
		return -1;
	byte_offset = sector * sector_size;
	total = (size_t)count * sector_size;
	while (done < total) {
		ssize_t result;
		off_t offset = (off_t)(byte_offset + done);
		if (write_data) {
			result = pwrite(device->fd, (const char *)buffer + done, total - done, offset);
		} else {
			result = pread(device->fd, (char *)buffer + done, total - done, offset);
		}
		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0) {
			record_io_error(device, write_data ? "write" : "read", errno);
			return -1;
		}
		if (result == 0) {
			record_io_error(device, write_data ? "write" : "read", EIO);
			return -1;
		}
		done += (size_t)result;
	}
	return 0;
}

static int block_device_read(
    void *context, uint64_t first_sector, uint32_t sector_count, void *buffer)
{
	return transfer(context, buffer, first_sector, sector_count, 0);
}

static int block_device_write(
    void *context, uint64_t first_sector, uint32_t sector_count, const void *buffer)
{
	return transfer(context, (void *)buffer, first_sector, sector_count, 1);
}

static int block_device_sync(void *context)
{
	struct device *device = context;
	int result;

#if defined(__APPLE__)
	if (device->is_regular_file) {
		do {
			result = fcntl(device->fd, F_FULLFSYNC);
		} while (result != 0 && errno == EINTR);
	} else {
		dk_synchronize_t request = {
			.offset = 0, .length = 0, .options = DK_SYNCHRONIZE_OPTION_BARRIER
		};

		do {
			result = ioctl(device->fd, DKIOCSYNCHRONIZE, &request);
		} while (result != 0 && errno == EINTR);
	}
#else
	do {
		result = fsync(device->fd);
	} while (result != 0 && errno == EINTR);
#endif
	if (result != 0)
		record_io_error(device, "synchronize", errno);
	return result;
}
