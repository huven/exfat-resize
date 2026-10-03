# SPDX-License-Identifier: MIT

# Dismount-DiskImage can spend about three minutes completing after a raw
# partition-layout update. Use the virtual-disk API directly so the persistence
# check does not depend on the Storage provider's removal notifications.
if (-not ('ExfatResizeTests.VirtualDisk' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace ExfatResizeTests
{
    public static class VirtualDisk
    {
        [StructLayout(LayoutKind.Sequential)]
        private struct StorageType
        {
            public uint DeviceId;
            public Guid VendorId;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct OpenParameters
        {
            public uint Version;
            public int GetInfoOnly;
            public int ReadOnly;
            public Guid ResiliencyGuid;
        }

        [DllImport("virtdisk.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern uint OpenVirtualDisk(ref StorageType storageType,
            string path, uint access, uint flags, ref OpenParameters parameters,
            out IntPtr handle);

        [DllImport("virtdisk.dll", ExactSpelling = true)]
        private static extern uint DetachVirtualDisk(SafeFileHandle handle,
            uint flags, uint providerFlags);

        public static void Detach(string path)
        {
            // Let Windows infer the image format. Version 2 requires ACCESS_NONE;
            // zero GetInfoOnly/ReadOnly values allow detaching the attached VHDX.
            StorageType storageType = new StorageType();
            OpenParameters parameters = new OpenParameters { Version = 2 };
            IntPtr rawHandle;
            uint status = OpenVirtualDisk(ref storageType, path, 0, 0,
                ref parameters, out rawHandle);
            if (status != 0)
                throw new Win32Exception((int)status,
                    "OpenVirtualDisk failed (Win32 " + status + "): " + path);
            using (SafeFileHandle handle = new SafeFileHandle(rawHandle, true))
            {
                status = DetachVirtualDisk(handle, 0, 0);
                if (status != 0)
                    throw new Win32Exception((int)status,
                        "DetachVirtualDisk failed (Win32 " + status + "): " + path);
            }
        }
    }
}
'@
}

function Dismount-TestDiskImage {
    param([string] $Image)

    $Timer = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Host "Detaching VHDX through DetachVirtualDisk: $Image"
    [ExfatResizeTests.VirtualDisk]::Detach($Image)
    Write-Host "DetachVirtualDisk returned after $($Timer.Elapsed.TotalSeconds.ToString('F2')) s"
    # A successful API return alone is insufficient for a persistence test:
    # Mount-DiskImage must attach afresh, rather than reuse an existing attachment.
    for ($Attempt = 0; $Attempt -lt 50; ++$Attempt) {
        if (-not (Get-DiskImage -ImagePath $Image).Attached) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Virtual disk is still attached after DetachVirtualDisk: $Image"
}
