/* SPDX-License-Identifier: MIT */

#include "windows/device_internal.h"
#include "windows/partition.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct device_partition_shrink {
	struct windows_partition partition;
	DRIVE_LAYOUT_INFORMATION_EX *scratch;
	uint64_t target_size;
};

void device_free_partition_shrink(struct device_partition_shrink *plan)
{
	if (plan == NULL)
		return;
	windows_partition_close(&plan->partition);
	free(plan->scratch);
	free(plan);
}

int device_prepare_partition_shrink(struct device *device,
    const char *path,
    uint64_t target_size,
    struct device_partition_shrink **output,
    char *error,
    size_t error_size)
{
	struct device_partition_shrink *plan;
	const PARTITION_INFORMATION_EX *entry;
	DWORD index;
	*output = NULL;
	plan = calloc(1, sizeof(*plan));
	if (plan == NULL) {
		windows_device_set_error(error, error_size, path, "cannot allocate partition shrink state");
		return -1;
	}
	if (windows_partition_open(device, path, &plan->partition, error, error_size) != 0) {
		free(plan);
		return -1;
	}
	entry = &plan->partition.layout->PartitionEntry[plan->partition.partition_index];
	if (target_size == 0 || target_size > (uint64_t)LLONG_MAX ||
	    target_size % device->block_device.sector_size != 0 ||
	    target_size >= (uint64_t)entry->PartitionLength.QuadPart) {
		windows_device_set_error(error, error_size, path,
		    "--shrink-partition requires a sector-aligned target smaller than the partition");
		goto fail;
	}
	for (index = 0; index < plan->partition.layout->PartitionCount; ++index) {
		const PARTITION_INFORMATION_EX *candidate = &plan->partition.layout->PartitionEntry[index];
		if (candidate->PartitionStyle != entry->PartitionStyle ||
		    candidate->StartingOffset.QuadPart < 0 || candidate->PartitionLength.QuadPart < 0 ||
		    (entry->PartitionStyle == PARTITION_STYLE_MBR &&
		        IsContainerPartition(candidate->Mbr.PartitionType))) {
			windows_device_set_error(error, error_size, path,
			    "partition shrink requires a basic layout without extended partitions or invalid "
			    "entries");
			goto fail;
		}
	}
	plan->scratch = malloc(DRIVE_LAYOUT_BUFFER_SIZE);
	if (plan->scratch == NULL) {
		windows_device_set_error(error, error_size, path, "cannot allocate disk layout readback");
		goto fail;
	}
	plan->target_size = target_size;
	*output = plan;
	return 0;
fail:
	device_free_partition_shrink(plan);
	return -1;
}

/* The original volume handle can disappear during a layout change. Enumerate
 * fresh handles and match disk extent plus partition identity, never a drive letter. */
static int verify_new_volume(const struct device_partition_shrink *plan)
{
	const PARTITION_INFORMATION_EX *expected =
	    &plan->partition.layout->PartitionEntry[plan->partition.partition_index];
	unsigned int attempt;
	for (attempt = 0; attempt < 50; ++attempt) {
		char name[MAX_PATH];
		HANDLE enumeration = FindFirstVolumeA(name, sizeof(name));
		if (enumeration != INVALID_HANDLE_VALUE) {
			do {
				HANDLE volume;
				DISK_EXTENT extent;
				PARTITION_INFORMATION_EX entry;
				GET_LENGTH_INFORMATION length;
				DWORD returned;
				char ignored[256];
				size_t size = strlen(name);
				int matches;
				if (size == 0 || name[size - 1] != '\\')
					continue;
				name[size - 1] = '\0';
				volume = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
				    OPEN_EXISTING, 0, NULL);
				if (volume == INVALID_HANDLE_VALUE)
					continue;
				matches = windows_partition_query_extent(
				              volume, name, &extent, ignored, sizeof(ignored)) == 0 &&
				    extent.DiskNumber == plan->partition.disk_number &&
				    extent.StartingOffset.QuadPart == expected->StartingOffset.QuadPart &&
				    extent.ExtentLength.QuadPart == expected->PartitionLength.QuadPart &&
				    windows_partition_query_information(
				        volume, name, &entry, ignored, sizeof(ignored)) == 0 &&
				    windows_partition_same_entry(&entry, expected) &&
				    DeviceIoControl(volume, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &length,
				        sizeof(length), &returned, NULL) &&
				    returned >= sizeof(length) &&
				    length.Length.QuadPart == expected->PartitionLength.QuadPart;
				(void)CloseHandle(volume);
				if (matches) {
					(void)FindVolumeClose(enumeration);
					return 0;
				}
			} while (FindNextVolumeA(enumeration, name, sizeof(name)));
			(void)FindVolumeClose(enumeration);
		}
		Sleep(100);
	}
	return -1;
}

int device_shrink_partition(struct device *device,
    const char *path,
    struct device_partition_shrink *plan,
    enum device_partition_state *state,
    char *error,
    size_t error_size)
{
	struct windows_partition *partition = &plan->partition;
	DWORD index, returned, error_number;
	DWORD layout_size;
	BOOL succeeded;
	*state = DEVICE_PARTITION_UNCHANGED;
	/* The driver may still cache the original filesystem geometry. Dismount
	 * even when subsequent layout revalidation rejects the partition update. */
	if (device_dismount(device, path, error, error_size) != 0)
		return -1;
	/* Revalidate the whole layout immediately before publishing it, so a stale
	 * preflight snapshot never overwrites another partition change. */
	if (windows_partition_read_layout(partition->disk, path, plan->scratch, error, error_size) != 0)
		return -1;
	if (!windows_partition_same_layout(partition->layout, plan->scratch)) {
		windows_device_set_error(error, error_size, path,
		    "the disk layout changed during filesystem shrink; partition left unchanged");
		return -1;
	}
	for (index = 0; index < partition->layout->PartitionCount; ++index)
		partition->layout->PartitionEntry[index].RewritePartition = FALSE;
	partition->layout->PartitionEntry[partition->partition_index].PartitionLength.QuadPart =
	    (LONGLONG)plan->target_size;
	partition->layout->PartitionEntry[partition->partition_index].RewritePartition = TRUE;
	layout_size = (DWORD)(offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) +
	    partition->layout->PartitionCount * sizeof(partition->layout->PartitionEntry[0]));
	*state = DEVICE_PARTITION_UPDATE_ATTEMPTED;
	succeeded = DeviceIoControl(partition->disk, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, partition->layout,
	    layout_size, NULL, 0, &returned, NULL);
	error_number = GetLastError();
	/* Never issue further I/O through a potentially invalidated volume handle. */
	device_close(device);
	if (!succeeded) {
		windows_device_set_operation_error(
		    error, error_size, path, "cannot shrink the partition", error_number);
		return -1;
	}
	if (!FlushFileBuffers(partition->disk)) {
		windows_device_set_operation_error(error, error_size, path,
		    "cannot synchronize the smaller partition table", GetLastError());
		return -1;
	}
	if (!DeviceIoControl(
	        partition->disk, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &returned, NULL)) {
		windows_device_set_operation_error(
		    error, error_size, path, "cannot refresh the smaller physical disk", GetLastError());
		return -1;
	}
	if (windows_partition_read_layout(partition->disk, path, plan->scratch, error, error_size) != 0)
		return -1;
	if (!windows_partition_same_layout(partition->layout, plan->scratch)) {
		windows_device_set_error(
		    error, error_size, path, "the smaller partition layout did not match readback");
		return -1;
	}
	*state = DEVICE_PARTITION_SHRUNK;
	if (verify_new_volume(plan) != 0) {
		windows_device_set_error(error, error_size, path,
		    "the smaller partition is verified, but its new volume device is not available");
		return -1;
	}
	return 0;
}
