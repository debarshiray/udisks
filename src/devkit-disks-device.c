/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 David Zeuthen <david@fubar.dk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <linux/fs.h>
#include <sys/ioctl.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include <glib-object.h>
#include <gio/gunixmounts.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <gudev/gudev.h>
#include <atasmart.h>

#include "devkit-disks-daemon.h"
#include "devkit-disks-device.h"
#include "devkit-disks-device-private.h"
#include "devkit-disks-marshal.h"
#include "devkit-disks-mount.h"
#include "devkit-disks-mount-monitor.h"
#include "devkit-disks-mount-file.h"
#include "devkit-disks-inhibitor.h"
#include "devkit-disks-poller.h"

/*--------------------------------------------------------------------------------------------------------------*/
#include "devkit-disks-device-glue.h"

static void     devkit_disks_device_class_init  (DevkitDisksDeviceClass *klass);
static void     devkit_disks_device_init        (DevkitDisksDevice      *seat);
static void     devkit_disks_device_finalize    (GObject     *object);

static void     polling_inhibitor_disconnected_cb (DevkitDisksInhibitor *inhibitor,
                                                   DevkitDisksDevice   *device);

static void     spindown_inhibitor_disconnected_cb (DevkitDisksInhibitor *inhibitor,
                                                    DevkitDisksDevice   *device);

static gboolean update_info                (DevkitDisksDevice *device);

static void     drain_pending_changes (DevkitDisksDevice *device, gboolean force_update);

static gboolean devkit_disks_device_local_is_busy (DevkitDisksDevice *device,
                                                   gboolean           check_partitions,
                                                   GError           **error);

static gboolean devkit_disks_device_local_partitions_are_busy (DevkitDisksDevice *device);
static gboolean devkit_disks_device_local_logical_partitions_are_busy (DevkitDisksDevice *device);


static gboolean luks_get_uid_from_dm_name (const char *dm_name, uid_t *out_uid);

/* Returns the cleartext device. If device==NULL, unlocking failed and an error has
 * been reported back to the caller
 */
typedef void (*UnlockEncryptionHookFunc) (DBusGMethodInvocation *context,
                                          DevkitDisksDevice *device,
                                          gpointer user_data);

static gboolean devkit_disks_device_luks_unlock_internal (DevkitDisksDevice        *device,
                                                               const char               *secret,
                                                               char                    **options,
                                                               UnlockEncryptionHookFunc  hook_func,
                                                               gpointer                  hook_user_data,
                                                               DBusGMethodInvocation    *context);

/* if filesystem_create_succeeded==FALSE, mkfs failed and an error has been reported back to the caller */
typedef void (*FilesystemCreateHookFunc) (DBusGMethodInvocation *context,
                                          DevkitDisksDevice *device,
                                          gboolean filesystem_create_succeeded,
                                          gpointer user_data);

static gboolean
devkit_disks_device_filesystem_create_internal (DevkitDisksDevice       *device,
                                                const char              *fstype,
                                                char                   **options,
                                                FilesystemCreateHookFunc hook_func,
                                                gpointer                 hook_user_data,
                                                DBusGMethodInvocation *context);

typedef void (*ForceRemovalCompleteFunc)     (DevkitDisksDevice        *device,
                                              gboolean                  success,
                                              gpointer                  user_data);

static void force_removal                    (DevkitDisksDevice        *device,
                                              ForceRemovalCompleteFunc  callback,
                                              gpointer                  user_data);

static void force_unmount                    (DevkitDisksDevice        *device,
                                              ForceRemovalCompleteFunc  callback,
                                              gpointer                  user_data);

static void force_luks_teardown            (DevkitDisksDevice        *device,
                                              DevkitDisksDevice        *cleartext_device,
                                              ForceRemovalCompleteFunc  callback,
                                              gpointer                  user_data);

enum
{
        PROP_0,
        PROP_NATIVE_PATH,

        PROP_DEVICE_DETECTION_TIME,
        PROP_DEVICE_MEDIA_DETECTION_TIME,
        PROP_DEVICE_MAJOR,
        PROP_DEVICE_MINOR,
        PROP_DEVICE_FILE,
        PROP_DEVICE_FILE_BY_ID,
        PROP_DEVICE_FILE_BY_PATH,
        PROP_DEVICE_IS_SYSTEM_INTERNAL,
        PROP_DEVICE_IS_PARTITION,
        PROP_DEVICE_IS_PARTITION_TABLE,
        PROP_DEVICE_IS_REMOVABLE,
        PROP_DEVICE_IS_MEDIA_AVAILABLE,
        PROP_DEVICE_IS_MEDIA_CHANGE_DETECTED,
        PROP_DEVICE_IS_MEDIA_CHANGE_DETECTION_POLLING,
        PROP_DEVICE_IS_MEDIA_CHANGE_DETECTION_INHIBITABLE,
        PROP_DEVICE_IS_MEDIA_CHANGE_DETECTION_INHIBITED,
        PROP_DEVICE_IS_READ_ONLY,
        PROP_DEVICE_IS_DRIVE,
        PROP_DEVICE_IS_OPTICAL_DISC,
        PROP_DEVICE_IS_LUKS,
        PROP_DEVICE_IS_LUKS_CLEARTEXT,
        PROP_DEVICE_IS_LINUX_MD_COMPONENT,
        PROP_DEVICE_IS_LINUX_MD,
        PROP_DEVICE_SIZE,
        PROP_DEVICE_BLOCK_SIZE,
        PROP_DEVICE_IS_MOUNTED,
        PROP_DEVICE_MOUNT_PATHS,
        PROP_DEVICE_MOUNTED_BY_UID,
        PROP_DEVICE_PRESENTATION_HIDE,
        PROP_DEVICE_PRESENTATION_NOPOLICY,
        PROP_DEVICE_PRESENTATION_NAME,
        PROP_DEVICE_PRESENTATION_ICON_NAME,

        PROP_JOB_IN_PROGRESS,
        PROP_JOB_ID,
        PROP_JOB_INITIATED_BY_UID,
        PROP_JOB_IS_CANCELLABLE,
        PROP_JOB_PERCENTAGE,

        PROP_ID_USAGE,
        PROP_ID_TYPE,
        PROP_ID_VERSION,
        PROP_ID_UUID,
        PROP_ID_LABEL,

        PROP_PARTITION_SLAVE,
        PROP_PARTITION_SCHEME,
        PROP_PARTITION_TYPE,
        PROP_PARTITION_LABEL,
        PROP_PARTITION_UUID,
        PROP_PARTITION_FLAGS,
        PROP_PARTITION_NUMBER,
        PROP_PARTITION_OFFSET,
        PROP_PARTITION_SIZE,

        PROP_PARTITION_TABLE_SCHEME,
        PROP_PARTITION_TABLE_COUNT,

        PROP_LUKS_HOLDER,

        PROP_LUKS_CLEARTEXT_SLAVE,
        PROP_LUKS_CLEARTEXT_UNLOCKED_BY_UID,

        PROP_DRIVE_VENDOR,
        PROP_DRIVE_MODEL,
        PROP_DRIVE_REVISION,
        PROP_DRIVE_SERIAL,
        PROP_DRIVE_CONNECTION_INTERFACE,
        PROP_DRIVE_CONNECTION_SPEED,
        PROP_DRIVE_MEDIA_COMPATIBILITY,
        PROP_DRIVE_MEDIA,
        PROP_DRIVE_IS_MEDIA_EJECTABLE,
        PROP_DRIVE_CAN_DETACH,
        PROP_DRIVE_CAN_SPINDOWN,
        PROP_DRIVE_IS_ROTATIONAL,

        PROP_OPTICAL_DISC_IS_BLANK,
        PROP_OPTICAL_DISC_IS_APPENDABLE,
        PROP_OPTICAL_DISC_IS_CLOSED,
        PROP_OPTICAL_DISC_NUM_TRACKS,
        PROP_OPTICAL_DISC_NUM_AUDIO_TRACKS,
        PROP_OPTICAL_DISC_NUM_SESSIONS,

        PROP_DRIVE_ATA_SMART_IS_AVAILABLE,
        PROP_DRIVE_ATA_SMART_TIME_COLLECTED,
        PROP_DRIVE_ATA_SMART_STATUS,
        PROP_DRIVE_ATA_SMART_BLOB,

        PROP_LINUX_MD_COMPONENT_LEVEL,
        PROP_LINUX_MD_COMPONENT_NUM_RAID_DEVICES,
        PROP_LINUX_MD_COMPONENT_UUID,
        PROP_LINUX_MD_COMPONENT_HOME_HOST,
        PROP_LINUX_MD_COMPONENT_NAME,
        PROP_LINUX_MD_COMPONENT_VERSION,
        PROP_LINUX_MD_COMPONENT_HOLDER,
        PROP_LINUX_MD_COMPONENT_STATE,

        PROP_LINUX_MD_STATE,
        PROP_LINUX_MD_LEVEL,
        PROP_LINUX_MD_NUM_RAID_DEVICES,
        PROP_LINUX_MD_UUID,
        PROP_LINUX_MD_HOME_HOST,
        PROP_LINUX_MD_NAME,
        PROP_LINUX_MD_VERSION,
        PROP_LINUX_MD_SLAVES,
        PROP_LINUX_MD_IS_DEGRADED,
        PROP_LINUX_MD_SYNC_ACTION,
        PROP_LINUX_MD_SYNC_PERCENTAGE,
        PROP_LINUX_MD_SYNC_SPEED,
};

enum
{
        CHANGED_SIGNAL,
        JOB_CHANGED_SIGNAL,
        LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (DevkitDisksDevice, devkit_disks_device, G_TYPE_OBJECT)

#define DEVKIT_DISKS_DEVICE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DEVKIT_DISKS_TYPE_DEVICE, DevkitDisksDevicePrivate))

static GObject *
devkit_disks_device_constructor (GType                  type,
                                 guint                  n_construct_properties,
                                 GObjectConstructParam *construct_properties)
{
        DevkitDisksDevice      *device;
        DevkitDisksDeviceClass *klass;

        klass = DEVKIT_DISKS_DEVICE_CLASS (g_type_class_peek (DEVKIT_DISKS_TYPE_DEVICE));

        device = DEVKIT_DISKS_DEVICE (
                G_OBJECT_CLASS (devkit_disks_device_parent_class)->constructor (type,
                                                                                n_construct_properties,
                                                                                construct_properties));
        return G_OBJECT (device);
}

static void
get_property (GObject         *object,
              guint            prop_id,
              GValue          *value,
              GParamSpec      *pspec)
{
        DevkitDisksDevice *device = DEVKIT_DISKS_DEVICE (object);

        switch (prop_id) {
        case PROP_NATIVE_PATH:
                g_value_set_string (value, device->priv->native_path);
                break;

        case PROP_DEVICE_DETECTION_TIME:
                g_value_set_uint64 (value, device->priv->device_detection_time);
                break;
        case PROP_DEVICE_MEDIA_DETECTION_TIME:
                g_value_set_uint64 (value, device->priv->device_media_detection_time);
                break;
        case PROP_DEVICE_MAJOR:
                g_value_set_int64 (value, major (device->priv->dev));
                break;
        case PROP_DEVICE_MINOR:
                g_value_set_int64 (value, minor (device->priv->dev));
                break;
        case PROP_DEVICE_FILE:
                g_value_set_string (value, device->priv->device_file);
                break;
        case PROP_DEVICE_FILE_BY_ID:
                g_value_set_boxed (value, device->priv->device_file_by_id);
                break;
        case PROP_DEVICE_FILE_BY_PATH:
                g_value_set_boxed (value, device->priv->device_file_by_path);
                break;
	case PROP_DEVICE_IS_SYSTEM_INTERNAL:
		g_value_set_boolean (value, device->priv->device_is_system_internal);
		break;
	case PROP_DEVICE_IS_PARTITION:
		g_value_set_boolean (value, device->priv->device_is_partition);
		break;
	case PROP_DEVICE_IS_PARTITION_TABLE:
		g_value_set_boolean (value, device->priv->device_is_partition_table);
		break;
	case PROP_DEVICE_IS_REMOVABLE:
		g_value_set_boolean (value, device->priv->device_is_removable);
		break;
	case PROP_DEVICE_IS_MEDIA_AVAILABLE:
		g_value_set_boolean (value, device->priv->device_is_media_available);
		break;
	case PROP_DEVICE_IS_MEDIA_CHANGE_DETECTED:
		g_value_set_boolean (value, device->priv->device_is_media_change_detected);
		break;
	case PROP_DEVICE_IS_MEDIA_CHANGE_DETECTION_POLLING:
		g_value_set_boolean (value, device->priv->device_is_media_change_detection_polling);
		break;
	case PROP_DEVICE_IS_MEDIA_CHANGE_DETECTION_INHIBITABLE:
		g_value_set_boolean (value, device->priv->device_is_media_change_detection_inhibitable);
		break;
	case PROP_DEVICE_IS_MEDIA_CHANGE_DETECTION_INHIBITED:
		g_value_set_boolean (value, device->priv->device_is_media_change_detection_inhibited);
		break;
	case PROP_DEVICE_IS_READ_ONLY:
		g_value_set_boolean (value, device->priv->device_is_read_only);
		break;
	case PROP_DEVICE_IS_DRIVE:
		g_value_set_boolean (value, device->priv->device_is_drive);
		break;
	case PROP_DEVICE_IS_OPTICAL_DISC:
		g_value_set_boolean (value, device->priv->device_is_optical_disc);
		break;
	case PROP_DEVICE_IS_LUKS:
		g_value_set_boolean (value, device->priv->device_is_luks);
		break;
	case PROP_DEVICE_IS_LUKS_CLEARTEXT:
		g_value_set_boolean (value, device->priv->device_is_luks_cleartext);
		break;
	case PROP_DEVICE_IS_LINUX_MD_COMPONENT:
		g_value_set_boolean (value, device->priv->device_is_linux_md_component);
		break;
	case PROP_DEVICE_IS_LINUX_MD:
		g_value_set_boolean (value, device->priv->device_is_linux_md);
		break;
	case PROP_DEVICE_SIZE:
		g_value_set_uint64 (value, device->priv->device_size);
		break;
	case PROP_DEVICE_BLOCK_SIZE:
		g_value_set_uint64 (value, device->priv->device_block_size);
		break;
	case PROP_DEVICE_IS_MOUNTED:
		g_value_set_boolean (value, device->priv->device_is_mounted);
		break;
	case PROP_DEVICE_MOUNT_PATHS:
		g_value_set_boxed (value, device->priv->device_mount_paths);
		break;
	case PROP_DEVICE_MOUNTED_BY_UID:
		g_value_set_uint (value, device->priv->device_mounted_by_uid);
		break;
	case PROP_DEVICE_PRESENTATION_HIDE:
		g_value_set_boolean (value, device->priv->device_presentation_hide);
		break;
	case PROP_DEVICE_PRESENTATION_NOPOLICY:
		g_value_set_boolean (value, device->priv->device_presentation_nopolicy);
		break;
	case PROP_DEVICE_PRESENTATION_NAME:
		g_value_set_string (value, device->priv->device_presentation_name);
		break;
	case PROP_DEVICE_PRESENTATION_ICON_NAME:
		g_value_set_string (value, device->priv->device_presentation_icon_name);
		break;

	case PROP_JOB_IN_PROGRESS:
		g_value_set_boolean (value, device->priv->job_in_progress);
		break;
	case PROP_JOB_ID:
		g_value_set_string (value, device->priv->job_id);
		break;
	case PROP_JOB_INITIATED_BY_UID:
		g_value_set_uint (value, device->priv->job_initiated_by_uid);
		break;
	case PROP_JOB_IS_CANCELLABLE:
		g_value_set_boolean (value, device->priv->job_is_cancellable);
		break;
	case PROP_JOB_PERCENTAGE:
		g_value_set_double (value, device->priv->job_percentage);
		break;

        case PROP_ID_USAGE:
                g_value_set_string (value, device->priv->id_usage);
                break;
        case PROP_ID_TYPE:
                g_value_set_string (value, device->priv->id_type);
                break;
        case PROP_ID_VERSION:
                g_value_set_string (value, device->priv->id_version);
                break;
        case PROP_ID_UUID:
                g_value_set_string (value, device->priv->id_uuid);
                break;
        case PROP_ID_LABEL:
                g_value_set_string (value, device->priv->id_label);
                break;

	case PROP_PARTITION_SLAVE:
                if (device->priv->partition_slave != NULL)
                        g_value_set_boxed (value, device->priv->partition_slave);
                else
                        g_value_set_boxed (value, "/");
		break;
	case PROP_PARTITION_SCHEME:
		g_value_set_string (value, device->priv->partition_scheme);
		break;
	case PROP_PARTITION_TYPE:
		g_value_set_string (value, device->priv->partition_type);
		break;
	case PROP_PARTITION_LABEL:
		g_value_set_string (value, device->priv->partition_label);
		break;
	case PROP_PARTITION_UUID:
		g_value_set_string (value, device->priv->partition_uuid);
		break;
	case PROP_PARTITION_FLAGS:
		g_value_set_boxed (value, device->priv->partition_flags);
		break;
	case PROP_PARTITION_NUMBER:
		g_value_set_int (value, device->priv->partition_number);
		break;
	case PROP_PARTITION_OFFSET:
		g_value_set_uint64 (value, device->priv->partition_offset);
		break;
	case PROP_PARTITION_SIZE:
		g_value_set_uint64 (value, device->priv->partition_size);
		break;

	case PROP_PARTITION_TABLE_SCHEME:
		g_value_set_string (value, device->priv->partition_table_scheme);
		break;
	case PROP_PARTITION_TABLE_COUNT:
		g_value_set_int (value, device->priv->partition_table_count);
		break;

	case PROP_LUKS_HOLDER:
                if (device->priv->luks_holder != NULL)
                        g_value_set_boxed (value, device->priv->luks_holder);
                else
                        g_value_set_boxed (value, "/");
		break;

	case PROP_LUKS_CLEARTEXT_SLAVE:
                if (device->priv->luks_cleartext_slave != NULL)
                        g_value_set_boxed (value, device->priv->luks_cleartext_slave);
                else
                        g_value_set_boxed (value, "/");
		break;
	case PROP_LUKS_CLEARTEXT_UNLOCKED_BY_UID:
		g_value_set_uint (value, device->priv->luks_cleartext_unlocked_by_uid);
		break;

	case PROP_DRIVE_VENDOR:
		g_value_set_string (value, device->priv->drive_vendor);
		break;
	case PROP_DRIVE_MODEL:
		g_value_set_string (value, device->priv->drive_model);
		break;
	case PROP_DRIVE_REVISION:
		g_value_set_string (value, device->priv->drive_revision);
		break;
	case PROP_DRIVE_SERIAL:
		g_value_set_string (value, device->priv->drive_serial);
		break;
	case PROP_DRIVE_CONNECTION_INTERFACE:
		g_value_set_string (value, device->priv->drive_connection_interface);
		break;
	case PROP_DRIVE_CONNECTION_SPEED:
		g_value_set_uint64 (value, device->priv->drive_connection_speed);
		break;
	case PROP_DRIVE_MEDIA_COMPATIBILITY:
		g_value_set_boxed (value, device->priv->drive_media_compatibility);
		break;
	case PROP_DRIVE_MEDIA:
		g_value_set_string (value, device->priv->drive_media);
		break;
	case PROP_DRIVE_IS_MEDIA_EJECTABLE:
		g_value_set_boolean (value, device->priv->drive_is_media_ejectable);
		break;
	case PROP_DRIVE_CAN_DETACH:
		g_value_set_boolean (value, device->priv->drive_can_detach);
		break;
	case PROP_DRIVE_CAN_SPINDOWN:
		g_value_set_boolean (value, device->priv->drive_can_spindown);
		break;
	case PROP_DRIVE_IS_ROTATIONAL:
		g_value_set_boolean (value, device->priv->drive_is_rotational);
		break;

	case PROP_OPTICAL_DISC_IS_BLANK:
		g_value_set_boolean (value, device->priv->optical_disc_is_blank);
		break;
	case PROP_OPTICAL_DISC_IS_APPENDABLE:
		g_value_set_boolean (value, device->priv->optical_disc_is_appendable);
		break;
	case PROP_OPTICAL_DISC_IS_CLOSED:
		g_value_set_boolean (value, device->priv->optical_disc_is_closed);
		break;
	case PROP_OPTICAL_DISC_NUM_TRACKS:
		g_value_set_uint (value, device->priv->optical_disc_num_tracks);
		break;
	case PROP_OPTICAL_DISC_NUM_AUDIO_TRACKS:
		g_value_set_uint (value, device->priv->optical_disc_num_audio_tracks);
		break;
	case PROP_OPTICAL_DISC_NUM_SESSIONS:
		g_value_set_uint (value, device->priv->optical_disc_num_sessions);
		break;

	case PROP_DRIVE_ATA_SMART_IS_AVAILABLE:
		g_value_set_boolean (value, device->priv->drive_ata_smart_is_available);
		break;
	case PROP_DRIVE_ATA_SMART_TIME_COLLECTED:
		g_value_set_uint64 (value, device->priv->drive_ata_smart_time_collected);
		break;
	case PROP_DRIVE_ATA_SMART_STATUS:
                {
                        const gchar *status;
                        if (device->priv->drive_ata_smart_status == (SkSmartOverall) -1)
                                status = "";
                        else
                                status = sk_smart_overall_to_string (device->priv->drive_ata_smart_status);
                        g_value_set_string (value, status);
                }
		break;
	case PROP_DRIVE_ATA_SMART_BLOB:
                {
                        GArray *a;
                        a = g_array_new (FALSE, FALSE, 1);
                        if (device->priv->drive_ata_smart_blob != NULL) {
                                g_array_append_vals (a,
                                                     device->priv->drive_ata_smart_blob,
                                                     device->priv->drive_ata_smart_blob_size);
                        }
                        g_value_set_boxed (value, a);
                        g_array_unref (a);
                }
		break;

	case PROP_LINUX_MD_COMPONENT_LEVEL:
		g_value_set_string (value, device->priv->linux_md_component_level);
		break;
	case PROP_LINUX_MD_COMPONENT_NUM_RAID_DEVICES:
		g_value_set_int (value, device->priv->linux_md_component_num_raid_devices);
		break;
	case PROP_LINUX_MD_COMPONENT_UUID:
		g_value_set_string (value, device->priv->linux_md_component_uuid);
		break;
	case PROP_LINUX_MD_COMPONENT_HOME_HOST:
		g_value_set_string (value, device->priv->linux_md_component_home_host);
		break;
	case PROP_LINUX_MD_COMPONENT_NAME:
		g_value_set_string (value, device->priv->linux_md_component_name);
		break;
	case PROP_LINUX_MD_COMPONENT_VERSION:
		g_value_set_string (value, device->priv->linux_md_component_version);
		break;
	case PROP_LINUX_MD_COMPONENT_HOLDER:
                if (device->priv->linux_md_component_holder != NULL)
                        g_value_set_boxed (value, device->priv->linux_md_component_holder);
                else
                        g_value_set_boxed (value, "/");
		break;
	case PROP_LINUX_MD_COMPONENT_STATE:
                g_value_set_boxed (value, device->priv->linux_md_component_state);
                break;

	case PROP_LINUX_MD_STATE:
		g_value_set_string (value, device->priv->linux_md_state);
		break;
	case PROP_LINUX_MD_LEVEL:
		g_value_set_string (value, device->priv->linux_md_level);
		break;
	case PROP_LINUX_MD_NUM_RAID_DEVICES:
		g_value_set_int (value, device->priv->linux_md_num_raid_devices);
		break;
	case PROP_LINUX_MD_UUID:
		g_value_set_string (value, device->priv->linux_md_uuid);
		break;
	case PROP_LINUX_MD_HOME_HOST:
		g_value_set_string (value, device->priv->linux_md_home_host);
		break;
	case PROP_LINUX_MD_NAME:
		g_value_set_string (value, device->priv->linux_md_name);
		break;
	case PROP_LINUX_MD_VERSION:
		g_value_set_string (value, device->priv->linux_md_version);
		break;
	case PROP_LINUX_MD_SLAVES:
                g_value_set_boxed (value, device->priv->linux_md_slaves);
                break;
	case PROP_LINUX_MD_IS_DEGRADED:
                g_value_set_boolean (value, device->priv->linux_md_is_degraded);
                break;
	case PROP_LINUX_MD_SYNC_ACTION:
                g_value_set_string (value, device->priv->linux_md_sync_action);
                break;
	case PROP_LINUX_MD_SYNC_PERCENTAGE:
                g_value_set_double (value, device->priv->linux_md_sync_percentage);
                break;
	case PROP_LINUX_MD_SYNC_SPEED:
                g_value_set_uint64 (value, device->priv->linux_md_sync_speed);
                break;

        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
devkit_disks_device_class_init (DevkitDisksDeviceClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->constructor = devkit_disks_device_constructor;
        object_class->finalize = devkit_disks_device_finalize;
        object_class->get_property = get_property;

        g_type_class_add_private (klass, sizeof (DevkitDisksDevicePrivate));

        signals[CHANGED_SIGNAL] =
                g_signal_new ("changed",
                              G_OBJECT_CLASS_TYPE (klass),
                              G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                              0,
                              NULL, NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE, 0);

        signals[JOB_CHANGED_SIGNAL] =
                g_signal_new ("job-changed",
                              G_OBJECT_CLASS_TYPE (klass),
                              G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                              0,
                              NULL, NULL,
                              devkit_disks_marshal_VOID__BOOLEAN_STRING_UINT_BOOLEAN_DOUBLE,
                              G_TYPE_NONE,
                              5,
                              G_TYPE_BOOLEAN,
                              G_TYPE_STRING,
                              G_TYPE_UINT,
                              G_TYPE_BOOLEAN,
                              G_TYPE_DOUBLE);

        dbus_g_object_type_install_info (DEVKIT_DISKS_TYPE_DEVICE, &dbus_glib_devkit_disks_device_object_info);

        g_object_class_install_property (
                object_class,
                PROP_NATIVE_PATH,
                g_param_spec_string ("native-path", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_DETECTION_TIME,
                g_param_spec_uint64 ("device-detection-time", NULL, NULL, 0, G_MAXUINT64, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_MEDIA_DETECTION_TIME,
                g_param_spec_uint64 ("device-media-detection-time", NULL, NULL, 0, G_MAXUINT64, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_MAJOR,
                g_param_spec_int64 ("device-major", NULL, NULL, -G_MAXINT64, G_MAXINT64, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_MINOR,
                g_param_spec_int64 ("device-minor", NULL, NULL, -G_MAXINT64, G_MAXINT64, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_FILE,
                g_param_spec_string ("device-file", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_FILE_BY_ID,
                g_param_spec_boxed ("device-file-by-id", NULL, NULL,
                                    dbus_g_type_get_collection ("GPtrArray", G_TYPE_STRING),
                                    G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_FILE_BY_PATH,
                g_param_spec_boxed ("device-file-by-path", NULL, NULL,
                                    dbus_g_type_get_collection ("GPtrArray", G_TYPE_STRING),
                                    G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_SYSTEM_INTERNAL,
                g_param_spec_boolean ("device-is-system-internal", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_PARTITION,
                g_param_spec_boolean ("device-is-partition", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_PARTITION_TABLE,
                g_param_spec_boolean ("device-is-partition-table", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_REMOVABLE,
                g_param_spec_boolean ("device-is-removable", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_MEDIA_AVAILABLE,
                g_param_spec_boolean ("device-is-media-available", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_MEDIA_CHANGE_DETECTED,
                g_param_spec_boolean ("device-is-media-change-detected", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_MEDIA_CHANGE_DETECTION_POLLING,
                g_param_spec_boolean ("device-is-media-change-detection-polling", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_MEDIA_CHANGE_DETECTION_INHIBITABLE,
                g_param_spec_boolean ("device-is-media-change-detection-inhibitable", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_MEDIA_CHANGE_DETECTION_INHIBITED,
                g_param_spec_boolean ("device-is-media-change-detection-inhibited", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_READ_ONLY,
                g_param_spec_boolean ("device-is-read-only", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_DRIVE,
                g_param_spec_boolean ("device-is-drive", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_OPTICAL_DISC,
                g_param_spec_boolean ("device-is-optical-disc", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_LUKS,
                g_param_spec_boolean ("device-is-luks", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_LUKS_CLEARTEXT,
                g_param_spec_boolean ("device-is-luks-cleartext", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_LINUX_MD_COMPONENT,
                g_param_spec_boolean ("device-is-linux-md-component", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_LINUX_MD,
                g_param_spec_boolean ("device-is-linux-md", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_SIZE,
                g_param_spec_uint64 ("device-size", NULL, NULL, 0, G_MAXUINT64, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_BLOCK_SIZE,
                g_param_spec_uint64 ("device-block-size", NULL, NULL, 0, G_MAXUINT64, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_IS_MOUNTED,
                g_param_spec_boolean ("device-is-mounted", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_MOUNT_PATHS,
                g_param_spec_boxed ("device-mount-paths", NULL, NULL,
                                    dbus_g_type_get_collection ("GPtrArray", G_TYPE_STRING),
                                    G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_MOUNTED_BY_UID,
                g_param_spec_uint ("device-mounted-by-uid", NULL, NULL, 0, G_MAXUINT, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_PRESENTATION_HIDE,
                g_param_spec_boolean ("device-presentation-hide", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_PRESENTATION_NOPOLICY,
                g_param_spec_boolean ("device-presentation-nopolicy", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_PRESENTATION_NAME,
                g_param_spec_string ("device-presentation-name", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DEVICE_PRESENTATION_ICON_NAME,
                g_param_spec_string ("device-presentation-icon-name", NULL, NULL, NULL, G_PARAM_READABLE));

        g_object_class_install_property (
                object_class,
                PROP_JOB_IN_PROGRESS,
                g_param_spec_boolean ("job-in-progress", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_JOB_ID,
                g_param_spec_string ("job-id", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_JOB_INITIATED_BY_UID,
                g_param_spec_uint ("job-initiated-by-uid", NULL, NULL, 0, G_MAXUINT, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_JOB_IS_CANCELLABLE,
                g_param_spec_boolean ("job-is-cancellable", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_JOB_PERCENTAGE,
                g_param_spec_double ("job-percentage", NULL, NULL, -1, 100, -1, G_PARAM_READABLE));

        g_object_class_install_property (
                object_class,
                PROP_ID_USAGE,
                g_param_spec_string ("id-usage", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_ID_TYPE,
                g_param_spec_string ("id-type", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_ID_VERSION,
                g_param_spec_string ("id-version", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_ID_UUID,
                g_param_spec_string ("id-uuid", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_ID_LABEL,
                g_param_spec_string ("id-label", NULL, NULL, NULL, G_PARAM_READABLE));

        g_object_class_install_property (
                object_class,
                PROP_PARTITION_SLAVE,
                g_param_spec_boxed ("partition-slave", NULL, NULL, DBUS_TYPE_G_OBJECT_PATH, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_PARTITION_SCHEME,
                g_param_spec_string ("partition-scheme", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_PARTITION_TYPE,
                g_param_spec_string ("partition-type", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_PARTITION_LABEL,
                g_param_spec_string ("partition-label", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_PARTITION_UUID,
                g_param_spec_string ("partition-uuid", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_PARTITION_FLAGS,
                g_param_spec_boxed ("partition-flags", NULL, NULL,
                                    dbus_g_type_get_collection ("GPtrArray", G_TYPE_STRING),
                                    G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_PARTITION_NUMBER,
                g_param_spec_int ("partition-number", NULL, NULL, 0, G_MAXINT, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_PARTITION_OFFSET,
                g_param_spec_uint64 ("partition-offset", NULL, NULL, 0, G_MAXUINT64, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_PARTITION_SIZE,
                g_param_spec_uint64 ("partition-size", NULL, NULL, 0, G_MAXUINT64, 0, G_PARAM_READABLE));

        g_object_class_install_property (
                object_class,
                PROP_PARTITION_TABLE_SCHEME,
                g_param_spec_string ("partition-table-scheme", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_PARTITION_TABLE_COUNT,
                g_param_spec_int ("partition-table-count", NULL, NULL, 0, G_MAXINT, 0, G_PARAM_READABLE));

        g_object_class_install_property (
                object_class,
                PROP_LUKS_HOLDER,
                g_param_spec_boxed ("luks-holder", NULL, NULL, DBUS_TYPE_G_OBJECT_PATH, G_PARAM_READABLE));

        g_object_class_install_property (
                object_class,
                PROP_LUKS_CLEARTEXT_SLAVE,
                g_param_spec_boxed ("luks-cleartext-slave", NULL, NULL, DBUS_TYPE_G_OBJECT_PATH, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LUKS_CLEARTEXT_UNLOCKED_BY_UID,
                g_param_spec_uint ("luks-cleartext-unlocked-by-uid", NULL, NULL, 0, G_MAXUINT, 0, G_PARAM_READABLE));

        g_object_class_install_property (
                object_class,
                PROP_DRIVE_VENDOR,
                g_param_spec_string ("drive-vendor", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_MODEL,
                g_param_spec_string ("drive-model", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_REVISION,
                g_param_spec_string ("drive-revision", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_SERIAL,
                g_param_spec_string ("drive-serial", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_CONNECTION_INTERFACE,
                g_param_spec_string ("drive-connection-interface", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_CONNECTION_SPEED,
                g_param_spec_uint64 ("drive-connection-speed", NULL, NULL, 0, G_MAXUINT64, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_MEDIA_COMPATIBILITY,
                g_param_spec_boxed ("drive-media-compatibility", NULL, NULL,
                                    dbus_g_type_get_collection ("GPtrArray", G_TYPE_STRING),
                                    G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_MEDIA,
                g_param_spec_string ("drive-media", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_IS_MEDIA_EJECTABLE,
                g_param_spec_boolean ("drive-is-media-ejectable", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_CAN_DETACH,
                g_param_spec_boolean ("drive-can-detach", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_CAN_SPINDOWN,
                g_param_spec_boolean ("drive-can-spindown", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_IS_ROTATIONAL,
                g_param_spec_boolean ("drive-is-rotational", NULL, NULL, FALSE, G_PARAM_READABLE));

        g_object_class_install_property (
                object_class,
                PROP_OPTICAL_DISC_IS_BLANK,
                g_param_spec_boolean ("optical-disc-is-blank", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_OPTICAL_DISC_IS_APPENDABLE,
                g_param_spec_boolean ("optical-disc-is-appendable", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_OPTICAL_DISC_IS_CLOSED,
                g_param_spec_boolean ("optical-disc-is-closed", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_OPTICAL_DISC_NUM_TRACKS,
                g_param_spec_uint ("optical-disc-num-tracks", NULL, NULL, 0, G_MAXUINT, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_OPTICAL_DISC_NUM_AUDIO_TRACKS,
                g_param_spec_uint ("optical-disc-num-audio-tracks", NULL, NULL, 0, G_MAXUINT, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_OPTICAL_DISC_NUM_SESSIONS,
                g_param_spec_uint ("optical-disc-num-sessions", NULL, NULL, 0, G_MAXUINT, 0, G_PARAM_READABLE));

        g_object_class_install_property (
                object_class,
                PROP_DRIVE_ATA_SMART_IS_AVAILABLE,
                g_param_spec_boolean ("drive-ata-smart-is-available", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_ATA_SMART_TIME_COLLECTED,
                g_param_spec_uint64 ("drive-ata-smart-time-collected", NULL, NULL, 0, G_MAXUINT64, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_ATA_SMART_STATUS,
                g_param_spec_string ("drive-ata-smart-status", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_DRIVE_ATA_SMART_BLOB,
                g_param_spec_boxed ("drive-ata-smart-blob", NULL, NULL,
                                    dbus_g_type_get_collection ("GArray", G_TYPE_UCHAR),
                                    G_PARAM_READABLE));


        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_COMPONENT_LEVEL,
                g_param_spec_string ("linux-md-component-level", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_COMPONENT_NUM_RAID_DEVICES,
                g_param_spec_int ("linux-md-component-num-raid-devices", NULL, NULL, 0, G_MAXINT, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_COMPONENT_UUID,
                g_param_spec_string ("linux-md-component-uuid", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_COMPONENT_HOME_HOST,
                g_param_spec_string ("linux-md-component-home-host", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_COMPONENT_NAME,
                g_param_spec_string ("linux-md-component-name", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_COMPONENT_VERSION,
                g_param_spec_string ("linux-md-component-version", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_COMPONENT_HOLDER,
                g_param_spec_boxed ("linux-md-component-holder", NULL, NULL, DBUS_TYPE_G_OBJECT_PATH, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_COMPONENT_STATE,
                g_param_spec_boxed ("linux-md-component-state", NULL, NULL,
                                    dbus_g_type_get_collection ("GPtrArray", G_TYPE_STRING),
                                    G_PARAM_READABLE));

        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_STATE,
                g_param_spec_string ("linux-md-state", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_LEVEL,
                g_param_spec_string ("linux-md-level", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_NUM_RAID_DEVICES,
                g_param_spec_int ("linux-md-num-raid-devices", NULL, NULL, 0, G_MAXINT, 0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_UUID,
                g_param_spec_string ("linux-md-uuid", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_HOME_HOST,
                g_param_spec_string ("linux-md-home-host", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_NAME,
                g_param_spec_string ("linux-md-name", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_VERSION,
                g_param_spec_string ("linux-md-version", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_SLAVES,
                g_param_spec_boxed ("linux-md-slaves", NULL, NULL,
                                    dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH),
                                    G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_IS_DEGRADED,
                g_param_spec_boolean ("linux-md-is-degraded", NULL, NULL, FALSE, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_SYNC_ACTION,
                g_param_spec_string ("linux-md-sync-action", NULL, NULL, NULL, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_SYNC_PERCENTAGE,
                g_param_spec_double ("linux-md-sync-percentage", NULL, NULL, 0.0, 100.0, 0.0, G_PARAM_READABLE));
        g_object_class_install_property (
                object_class,
                PROP_LINUX_MD_SYNC_SPEED,
                g_param_spec_uint64 ("linux-md-sync-speed", NULL, NULL, 0, G_MAXUINT64, 0, G_PARAM_READABLE));
}

static void
devkit_disks_device_init (DevkitDisksDevice *device)
{
        device->priv = DEVKIT_DISKS_DEVICE_GET_PRIVATE (device);

        device->priv->device_file_by_id = g_ptr_array_new ();
        device->priv->device_file_by_path = g_ptr_array_new ();
        device->priv->device_mount_paths = g_ptr_array_new ();
        device->priv->partition_flags = g_ptr_array_new ();
        device->priv->drive_media_compatibility = g_ptr_array_new ();
        device->priv->linux_md_component_state = g_ptr_array_new ();
        device->priv->linux_md_slaves = g_ptr_array_new ();
        device->priv->slaves_objpath = g_ptr_array_new ();
        device->priv->holders_objpath = g_ptr_array_new ();

        device->priv->drive_ata_smart_status = -1;
}

static void
devkit_disks_device_finalize (GObject *object)
{
        DevkitDisksDevice *device;
        GList *l;

        g_return_if_fail (object != NULL);
        g_return_if_fail (DEVKIT_DISKS_IS_DEVICE (object));

        device = DEVKIT_DISKS_DEVICE (object);
        g_return_if_fail (device->priv != NULL);

        g_debug ("finalizing %s", device->priv->native_path);

        g_object_unref (device->priv->d);
        g_object_unref (device->priv->daemon);
        g_free (device->priv->object_path);

        g_free (device->priv->native_path);

        for (l = device->priv->polling_inhibitors; l != NULL; l = l->next) {
                DevkitDisksInhibitor *inhibitor = DEVKIT_DISKS_INHIBITOR (l->data);
                g_signal_handlers_disconnect_by_func (inhibitor, polling_inhibitor_disconnected_cb, device);
                g_object_unref (inhibitor);
        }
        g_list_free (device->priv->polling_inhibitors);

        for (l = device->priv->spindown_inhibitors; l != NULL; l = l->next) {
                DevkitDisksInhibitor *inhibitor = DEVKIT_DISKS_INHIBITOR (l->data);
                g_signal_handlers_disconnect_by_func (inhibitor, spindown_inhibitor_disconnected_cb, device);
                g_object_unref (inhibitor);
        }
        g_list_free (device->priv->spindown_inhibitors);

        if (device->priv->linux_md_poll_timeout_id > 0)
                g_source_remove (device->priv->linux_md_poll_timeout_id);

        if (device->priv->emit_changed_idle_id > 0)
                g_source_remove (device->priv->emit_changed_idle_id);

        /* free properties */
        g_free (device->priv->device_file);
        g_ptr_array_foreach (device->priv->device_file_by_id, (GFunc) g_free, NULL);
        g_ptr_array_foreach (device->priv->device_file_by_path, (GFunc) g_free, NULL);
        g_ptr_array_free (device->priv->device_file_by_id, TRUE);
        g_ptr_array_free (device->priv->device_file_by_path, TRUE);
        g_ptr_array_free (device->priv->device_mount_paths, TRUE);
        g_free (device->priv->device_presentation_name);
        g_free (device->priv->device_presentation_icon_name);

        g_free (device->priv->id_usage);
        g_free (device->priv->id_type);
        g_free (device->priv->id_version);
        g_free (device->priv->id_uuid);
        g_free (device->priv->id_label);

        g_free (device->priv->partition_slave);
        g_free (device->priv->partition_scheme);
        g_free (device->priv->partition_type);
        g_free (device->priv->partition_label);
        g_free (device->priv->partition_uuid);
        g_ptr_array_foreach (device->priv->partition_flags, (GFunc) g_free, NULL);
        g_ptr_array_free (device->priv->partition_flags, TRUE);

        g_free (device->priv->partition_table_scheme);

        g_free (device->priv->luks_holder);

        g_free (device->priv->luks_cleartext_slave);

        g_free (device->priv->drive_vendor);
        g_free (device->priv->drive_model);
        g_free (device->priv->drive_revision);
        g_free (device->priv->drive_serial);
        g_free (device->priv->drive_connection_interface);
        g_ptr_array_foreach (device->priv->drive_media_compatibility, (GFunc) g_free, NULL);
        g_ptr_array_free (device->priv->drive_media_compatibility, TRUE);
        g_free (device->priv->drive_media);

        g_free (device->priv->linux_md_component_level);
        g_free (device->priv->linux_md_component_uuid);
        g_free (device->priv->linux_md_component_home_host);
        g_free (device->priv->linux_md_component_name);
        g_free (device->priv->linux_md_component_version);
        g_free (device->priv->linux_md_component_holder);
        g_ptr_array_foreach (device->priv->linux_md_component_state, (GFunc) g_free, NULL);
        g_ptr_array_free (device->priv->linux_md_component_state, TRUE);

        g_free (device->priv->linux_md_state);
        g_free (device->priv->linux_md_level);
        g_free (device->priv->linux_md_uuid);
        g_free (device->priv->linux_md_home_host);
        g_free (device->priv->linux_md_name);
        g_free (device->priv->linux_md_version);
        g_ptr_array_foreach (device->priv->linux_md_slaves, (GFunc) g_free, NULL);
        g_ptr_array_free (device->priv->linux_md_slaves, TRUE);

        g_free (device->priv->drive_ata_smart_blob);

        g_free (device->priv->dm_name);
        g_ptr_array_foreach (device->priv->slaves_objpath, (GFunc) g_free, NULL);
        g_ptr_array_free (device->priv->slaves_objpath, TRUE);
        g_ptr_array_foreach (device->priv->holders_objpath, (GFunc) g_free, NULL);
        g_ptr_array_free (device->priv->holders_objpath, TRUE);


        G_OBJECT_CLASS (devkit_disks_device_parent_class)->finalize (object);
}

/**
 * compute_object_path:
 * @native_path: Either an absolute sysfs path or the basename
 *
 * Maps @native_path to the D-Bus object path for the device.
 *
 * Returns: A valid D-Bus object path. Free with g_free().
 */
static char *
compute_object_path (const char *native_path)
{
        const gchar *basename;
        GString *s;
        guint n;

        basename = strrchr (native_path, '/');
        if (basename != NULL) {
                basename++;
        } else {
                basename = native_path;
        }

        s = g_string_new ("/org/freedesktop/DeviceKit/Disks/devices/");
        for (n = 0; basename[n] != '\0'; n++) {
                gint c = basename[n];

                /* D-Bus spec sez:
                 *
                 * Each element must only contain the ASCII characters "[A-Z][a-z][0-9]_"
                 */
                if ((c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9')) {
                        g_string_append_c (s, c);
                } else {
                        /* Escape bytes not in [A-Z][a-z][0-9] as _<hex-with-two-digits> */
                        g_string_append_printf (s, "_%02x", c);
                }
        }

        return g_string_free (s, FALSE);
}

static gboolean
register_disks_device (DevkitDisksDevice *device)
{
        DBusConnection *connection;
        GError *error = NULL;

        device->priv->system_bus_connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
        if (device->priv->system_bus_connection == NULL) {
                if (error != NULL) {
                        g_critical ("error getting system bus: %s", error->message);
                        g_error_free (error);
                }
                goto error;
        }
        connection = dbus_g_connection_get_connection (device->priv->system_bus_connection);

        device->priv->object_path = compute_object_path (device->priv->native_path);

        if (dbus_g_connection_lookup_g_object (device->priv->system_bus_connection,
                                               device->priv->object_path) != NULL) {
                /* TODO: see devkit_disks_device_removed() for where we want to unregister the object but
                 * we're missing the API. So do it manually here if we are forced to do so...
                 */

                g_print ("**** HACK: Wanting to register object at path `%s' but there is already an "
                         "object there. Using a hack to move it out of the way.\n",
                         device->priv->object_path);

                dbus_connection_unregister_object_path (dbus_g_connection_get_connection (device->priv->system_bus_connection),
                                                        device->priv->object_path);
        }

        dbus_g_connection_register_g_object (device->priv->system_bus_connection,
                                             device->priv->object_path,
                                             G_OBJECT (device));

        return TRUE;

error:
        return FALSE;
}

static double
sysfs_get_double (const char *dir, const char *attribute)
{
        double result;
        char *contents;
        char *filename;

        result = 0.0;
        filename = g_build_filename (dir, attribute, NULL);
        if (g_file_get_contents (filename, &contents, NULL, NULL)) {
                result = atof (contents);
                g_free (contents);
        }
        g_free (filename);


        return result;
}

static char *
sysfs_get_string (const char *dir, const char *attribute)
{
        char *result;
        char *filename;

        result = NULL;
        filename = g_build_filename (dir, attribute, NULL);
        if (!g_file_get_contents (filename, &result, NULL, NULL)) {
                result = g_strdup ("");
        }
        g_free (filename);

        return result;
}

static int
sysfs_get_int (const char *dir, const char *attribute)
{
        int result;
        char *contents;
        char *filename;

        result = 0;
        filename = g_build_filename (dir, attribute, NULL);
        if (g_file_get_contents (filename, &contents, NULL, NULL)) {
                result = strtol (contents, NULL, 0);
                g_free (contents);
        }
        g_free (filename);


        return result;
}

static guint64
sysfs_get_uint64 (const char *dir, const char *attribute)
{
        guint64 result;
        char *contents;
        char *filename;

        result = 0;
        filename = g_build_filename (dir, attribute, NULL);
        if (g_file_get_contents (filename, &contents, NULL, NULL)) {
                result = strtoll (contents, NULL, 0);
                g_free (contents);
        }
        g_free (filename);

        return result;
}

static gboolean
sysfs_file_exists (const char *dir, const char *attribute)
{
        gboolean result;
        char *filename;

        result = FALSE;
        filename = g_build_filename (dir, attribute, NULL);
        if (g_file_test (filename, G_FILE_TEST_EXISTS)) {
                result = TRUE;
        }
        g_free (filename);

        return result;
}

static void
devkit_disks_device_generate_kernel_change_event (DevkitDisksDevice *device)
{
        FILE *f;
        char *filename;

        filename = g_build_filename (device->priv->native_path, "uevent", NULL);
        f = fopen (filename, "w");
        if (f == NULL) {
                g_warning ("error opening %s for writing: %m", filename);
        } else {
                if (fputs ("change", f) == EOF) {
                        g_warning ("error writing 'change' to %s: %m", filename);
                }
                fclose (f);
        }
        g_free (filename);
}

static char *
_dupv8 (const char *s)
{
        const char *end_valid;

        if (!g_utf8_validate (s,
                             -1,
                             &end_valid)) {
                g_print ("**** NOTE: The string '%s' is not valid UTF-8. Invalid characters begins at '%s'\n",
                         s, end_valid);
                return g_strndup (s, end_valid - s);
        } else {
                return g_strdup (s);
        }
}

static char *
sysfs_resolve_link (const char *sysfs_path, const char *name)
{
        char *full_path;
        char link_path[PATH_MAX];
        char resolved_path[PATH_MAX];
        ssize_t num;
        gboolean found_it;

        found_it = FALSE;

        full_path = g_build_filename (sysfs_path, name, NULL);

        //g_debug ("name='%s'", name);
        //g_debug ("full_path='%s'", full_path);
        num = readlink (full_path, link_path, sizeof (link_path) - 1);
        if (num != -1) {
                char *absolute_path;

                link_path[num] = '\0';

                //g_debug ("link_path='%s'", link_path);
                absolute_path = g_build_filename (sysfs_path, link_path, NULL);
                //g_debug ("absolute_path='%s'", absolute_path);
                if (realpath (absolute_path, resolved_path) != NULL) {
                        //g_debug ("resolved_path='%s'", resolved_path);
                        found_it = TRUE;
                }
                g_free (absolute_path);
        }
        g_free (full_path);

        if (found_it)
                return g_strdup (resolved_path);
        else
                return NULL;
}

/* unescapes things like \x20 to " " and ensures the returned string is valid UTF-8.
 *
 * see volume_id_encode_string() in extras/volume_id/lib/volume_id.c in the
 * udev tree for the encoder
 */
static gchar *
decode_udev_encoded_string (const gchar *str)
{
        GString *s;
        gchar *ret;
        const gchar *end_valid;
        guint n;

        s = g_string_new (NULL);
        for (n = 0; str[n] != '\0'; n++) {
                if (str[n] == '\\') {
                        gint val;

                        if (str[n + 1] != 'x' || str[n + 2] == '\0' || str[n + 3] == '\0') {
                                g_print ("**** NOTE: malformed encoded string '%s'\n", str);
                                break;
                        }

                        val = (g_ascii_xdigit_value (str[n + 2]) << 4) | g_ascii_xdigit_value (str[n + 3]);

                        g_string_append_c (s, val);

                        n += 3;
                } else {
                        g_string_append_c (s, str[n]);
                }
        }

        if (!g_utf8_validate (s->str, -1, &end_valid)) {
                g_print ("**** NOTE: The string '%s' is not valid UTF-8. Invalid characters begins at '%s'\n",
                         s->str, end_valid);
                ret = g_strndup (s->str, end_valid - s->str);
                g_string_free (s, TRUE);
        } else {
                ret = g_string_free (s, FALSE);
        }

        return ret;
}

static gboolean
poll_syncing_md_device (gpointer user_data)
{
        DevkitDisksDevice *device = DEVKIT_DISKS_DEVICE (user_data);

        g_print ("**** POLL SYNCING MD %s\n", device->priv->native_path);

        device->priv->linux_md_poll_timeout_id = 0;
        devkit_disks_daemon_local_synthesize_changed (device->priv->daemon, device);
        return FALSE;
}

static GList *
dup_list_from_ptrarray (GPtrArray *p)
{
        GList *ret;
        guint n;

        ret = NULL;

        for (n = 0; n < p->len; n++)
                ret = g_list_prepend (ret, g_strdup (((gchar **) p->pdata) [n]));

        return ret;
}

static gint
ptr_str_array_compare (const gchar **a, const gchar **b)
{
        return g_strcmp0 (*a, *b);
}

static void
diff_sorted_lists (GList         *list1,
                   GList         *list2,
                   GCompareFunc   compare,
                   GList        **added,
                   GList        **removed)
{
  int order;

  *added = *removed = NULL;

  while (list1 != NULL &&
         list2 != NULL)
    {
      order = (*compare) (list1->data, list2->data);
      if (order < 0)
        {
          *removed = g_list_prepend (*removed, list1->data);
          list1 = list1->next;
        }
      else if (order > 0)
        {
          *added = g_list_prepend (*added, list2->data);
          list2 = list2->next;
        }
      else
        { /* same item */
          list1 = list1->next;
          list2 = list2->next;
        }
    }

  while (list1 != NULL)
    {
      *removed = g_list_prepend (*removed, list1->data);
      list1 = list1->next;
    }
  while (list2 != NULL)
    {
      *added = g_list_prepend (*added, list2->data);
      list2 = list2->next;
    }
}

/* ---------------------------------------------------------------------------------------------------- */

/* update id_* properties */
static gboolean
update_info_presentation (DevkitDisksDevice *device)
{
        gboolean hide;
        gboolean nopolicy;

        hide = FALSE;
        if (g_udev_device_has_property (device->priv->d, "DKD_PRESENTATION_HIDE"))
                hide = g_udev_device_get_property_as_boolean (device->priv->d, "DKD_PRESENTATION_HIDE");
        devkit_disks_device_set_device_presentation_hide (device, hide);

        nopolicy = FALSE;
        if (g_udev_device_has_property (device->priv->d, "DKD_PRESENTATION_NOPOLICY"))
                nopolicy = g_udev_device_get_property_as_boolean (device->priv->d, "DKD_PRESENTATION_NOPOLICY");
        devkit_disks_device_set_device_presentation_nopolicy (device, nopolicy);

        devkit_disks_device_set_device_presentation_name (device,
               g_udev_device_get_property (device->priv->d, "DKD_PRESENTATION_NAME"));

        devkit_disks_device_set_device_presentation_icon_name (device,
               g_udev_device_get_property (device->priv->d, "DKD_PRESENTATION_ICON_NAME"));

        return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/* update id_* properties */
static gboolean
update_info_id (DevkitDisksDevice *device)
{
        gchar *decoded_string;

        devkit_disks_device_set_id_usage (device, g_udev_device_get_property (device->priv->d, "ID_FS_USAGE"));
        devkit_disks_device_set_id_type (device, g_udev_device_get_property (device->priv->d, "ID_FS_TYPE"));
        devkit_disks_device_set_id_version (device, g_udev_device_get_property (device->priv->d, "ID_FS_VERSION"));
        if (g_udev_device_has_property (device->priv->d, "ID_FS_LABEL_ENC")) {
                decoded_string = decode_udev_encoded_string (g_udev_device_get_property (device->priv->d, "ID_FS_LABEL_ENC"));
                devkit_disks_device_set_id_label (device, decoded_string);
                g_free (decoded_string);
        } else {
                devkit_disks_device_set_id_label (device, g_udev_device_get_property (device->priv->d, "ID_FS_LABEL"));
        }
        devkit_disks_device_set_id_uuid (device, g_udev_device_get_property (device->priv->d, "ID_FS_UUID"));

        return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/* update partition_table_* properties */
static gboolean
update_info_partition_table (DevkitDisksDevice *device)
{
        if (!device->priv->device_is_partition &&
            g_udev_device_has_property (device->priv->d, "DKD_PARTITION_TABLE") &&
            g_udev_device_get_property_as_boolean (device->priv->d, "DKD_PARTITION_TABLE")) {

                /* Some times we think that vfat on the main block device looks like a Master Boot Record
                 * partition table (the on-disk formats are extremely similar). So if we already have
                 * detected a file system on the main block device and don't have any partitions, then
                 * avoid tagging the device as a partition table.
                 *
                 * See e.g. https://bugzilla.redhat.com/show_bug.cgi?id=495876.
                 */
                if (device->priv->partition_table_count == 0 &&
                    g_strcmp0 (device->priv->id_usage, "filesystem") == 0) {
                        devkit_disks_device_set_device_is_partition_table (device, FALSE);
                } else {
                        devkit_disks_device_set_device_is_partition_table (device, TRUE);
                        devkit_disks_device_set_partition_table_scheme (device, g_udev_device_get_property (device->priv->d, "DKD_PARTITION_TABLE_SCHEME"));
                }
        } else {
                devkit_disks_device_set_partition_table_scheme (device, NULL);
        }

        return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/* update partition_* properties */
static gboolean
update_info_partition (DevkitDisksDevice *device)
{
        guint64 offset;

        offset = sysfs_get_uint64 (device->priv->native_path, "start") * device->priv->device_block_size;
        devkit_disks_device_set_partition_offset (device, offset);

        if (device->priv->device_is_partition &&
            g_udev_device_has_property (device->priv->d, "DKD_PARTITION")) {
                guint64 size;
                const gchar *scheme;
                const gchar *type;
                const gchar *label;
                const gchar *uuid;
                const gchar* const *flags;

                scheme = g_udev_device_get_property (device->priv->d, "DKD_PARTITION_SCHEME");
                size = g_udev_device_get_property_as_uint64 (device->priv->d, "DKD_PARTITION_SIZE");
                type = g_udev_device_get_property (device->priv->d, "DKD_PARTITION_TYPE");
                label = g_udev_device_get_property (device->priv->d, "DKD_PARTITION_LABEL");
                uuid = g_udev_device_get_property (device->priv->d, "DKD_PARTITION_UUID");
                flags = g_udev_device_get_property_as_strv (device->priv->d, "DKD_PARTITION_FLAGS");

                devkit_disks_device_set_partition_scheme (device, scheme);
                devkit_disks_device_set_partition_size (device, size);
                devkit_disks_device_set_partition_type (device, type);
                devkit_disks_device_set_partition_label (device, label);
                devkit_disks_device_set_partition_uuid (device, uuid);
                devkit_disks_device_set_partition_flags (device, (gchar **) flags);
        } else {
                /* if we don't have info from part_id, set the partition size to the same as the block device */
                devkit_disks_device_set_partition_scheme (device, NULL);
                devkit_disks_device_set_partition_size (device, device->priv->device_size);
                devkit_disks_device_set_partition_type (device, NULL);
                devkit_disks_device_set_partition_label (device, NULL);
                devkit_disks_device_set_partition_uuid (device, NULL);
                devkit_disks_device_set_partition_flags (device, NULL);
        }

        return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/* this function sets
 *
 *  - drive_vendor (unless set already)
 *  - drive_model (unless set already)
 *  - connection_interface  (if we can figure that out)
 *  - connection_speed (if we can figure that out)
 *
 * All this should really come from udev properties but right now it isn't.
 */
static void
update_drive_properties_from_sysfs (DevkitDisksDevice *device)
{
        char *s;
        char *p;
        char *q;
        char *model;
        char *vendor;
        char *subsystem;
        char *serial;
        char *revision;
        const char *connection_interface;
        guint64 connection_speed;

        connection_interface = NULL;
        connection_speed = 0;

        /* walk up the device tree to figure out the subsystem */
        s = g_strdup (device->priv->native_path);
        do {
                p = sysfs_resolve_link (s, "subsystem");
                if (p != NULL) {
                        subsystem = g_path_get_basename (p);
                        g_free (p);

                        if (strcmp (subsystem, "scsi") == 0) {
                                connection_interface = "scsi";
                                connection_speed = 0;

                                /* continue walking up the chain; we just use scsi as a fallback */

                                /* grab the names from SCSI since the names from udev currently
                                 *  - replaces whitespace with _
                                 *  - is missing for e.g. Firewire
                                 */
                                vendor = sysfs_get_string (s, "vendor");
                                if (vendor != NULL) {
                                        g_strstrip (vendor);
                                        /* Don't overwrite what we set earlier from ID_VENDOR */
                                        if (device->priv->drive_vendor == NULL) {
                                                q = _dupv8 (vendor);
                                                devkit_disks_device_set_drive_vendor (device, q);
                                                g_free (q);
                                        }
                                        g_free (vendor);
                                }

                                model = sysfs_get_string (s, "model");
                                if (model != NULL) {
                                        g_strstrip (model);
                                        /* Don't overwrite what we set earlier from ID_MODEL */
                                        if (device->priv->drive_model == NULL) {
                                                q = _dupv8 (model);
                                                devkit_disks_device_set_drive_model (device, q);
                                                g_free (q);
                                        }
                                        g_free (model);
                                }

                                /* TODO: need to improve this code; we probably need the kernel to export more
                                 *       information before we can properly get the type and speed.
                                 */

                                if (device->priv->drive_vendor != NULL &&
                                    strcmp (device->priv->drive_vendor, "ATA") == 0) {
                                        connection_interface = "ata";
                                        break;
                                }

                        } else if (strcmp (subsystem, "usb") == 0) {
                                double usb_speed;

                                /* both the interface and the device will be 'usb'. However only
                                 * the device will have the 'speed' property.
                                 */
                                usb_speed = sysfs_get_double (s, "speed");
                                if (usb_speed > 0) {
                                        connection_interface = "usb";
                                        connection_speed = usb_speed * (1000 * 1000);
                                        break;

                                }
                        } else if (strcmp (subsystem, "firewire") == 0) {

                                /* TODO: krh has promised a speed file in sysfs; theoretically, the speed can
                                 *       be anything from 100, 200, 400, 800 and 3200. Till then we just hardcode
                                 *       a resonable default of 400 Mbit/s.
                                 */

                                connection_interface = "firewire";
                                connection_speed = 400 * (1000 * 1000);
                                break;

                        } else if (strcmp (subsystem, "mmc") == 0) {

                                /* TODO: what about non-SD, e.g. MMC? Is that another bus? */
                                connection_interface = "sdio";

                                /* Set vendor name. According to this MMC document
                                 *
                                 * http://www.mmca.org/membership/IAA_Agreement_10_12_06.pdf
                                 *
                                 *  - manfid: the manufacturer id
                                 *  - oemid: the customer of the manufacturer
                                 *
                                 * Apparently these numbers are kept secret. It would be nice
                                 * to map these into names for setting the manufacturer of the drive,
                                 * e.g. Panasonic, Sandisk etc.
                                 */

                                model = sysfs_get_string (s, "name");
                                if (model != NULL) {
                                        g_strstrip (model);
                                        /* Don't overwrite what we set earlier from ID_MODEL */
                                        if (device->priv->drive_model == NULL) {
                                                q = _dupv8 (model);
                                                devkit_disks_device_set_drive_model (device, q);
                                                g_free (q);
                                        }
                                        g_free (model);
                                }

                                serial = sysfs_get_string (s, "serial");
                                if (serial != NULL) {
                                        g_strstrip (serial);
                                        /* Don't overwrite what we set earlier from ID_SERIAL */
                                        if (device->priv->drive_serial == NULL) {
                                                /* this is formatted as a hexnumber; drop the leading 0x */
                                                q = _dupv8 (serial + 2);
                                                devkit_disks_device_set_drive_serial (device, q);
                                                g_free (q);
                                        }
                                        g_free (serial);
                                }

                                /* TODO: use hwrev and fwrev files? */
                                revision = sysfs_get_string (s, "date");
                                if (revision != NULL) {
                                        g_strstrip (revision);
                                        /* Don't overwrite what we set earlier from ID_REVISION */
                                        if (device->priv->drive_revision == NULL) {
                                                q = _dupv8 (revision);
                                                devkit_disks_device_set_drive_revision (device, q);
                                                g_free (q);
                                        }
                                        g_free (revision);
                                }

                                /* TODO: interface speed; the kernel driver knows; would be nice
                                 * if it could export it */

                        } else if (strcmp (subsystem, "platform") == 0) {
                                const gchar *sysfs_name;

                                sysfs_name = g_strrstr (s, "/");
                                if (g_str_has_prefix (sysfs_name + 1, "floppy.")) {
                                        devkit_disks_device_set_drive_vendor (device, "Floppy Drive");
                                        connection_interface = "platform";
                                }
                        }

                        g_free (subsystem);
                }

                /* advance up the chain */
                p = g_strrstr (s, "/");
                if (p == NULL)
                        break;
                *p = '\0';

                /* but stop at the root */
                if (strcmp (s, "/sys/devices") == 0)
                        break;

        } while (TRUE);

        if (connection_interface != NULL) {
                devkit_disks_device_set_drive_connection_interface (device, connection_interface);
                devkit_disks_device_set_drive_connection_speed (device, connection_speed);
        }

        g_free (s);
}

static const struct
{
        const gchar *udev_property;
        const gchar *media_name;
} drive_media_mapping[] = {
        {"ID_DRIVE_FLASH",          "flash"},
        {"ID_DRIVE_FLASH_CF",       "flash_cf"},
        {"ID_DRIVE_FLASH_MS",       "flash_ms"},
        {"ID_DRIVE_FLASH_SM",       "flash_sm"},
        {"ID_DRIVE_FLASH_SD",       "flash_sd"},
        {"ID_DRIVE_FLASH_SDHC",     "flash_sdhc"},
        {"ID_DRIVE_FLASH_MMC",      "flash_mmc"},
        {"ID_DRIVE_FLOPPY",         "floppy"},
        {"ID_DRIVE_FLOPPY_ZIP",     "floppy_zip"},
        {"ID_DRIVE_FLOPPY_JAZ",     "floppy_jaz"},
        {"ID_CDROM",                "optical_cd"},
        {"ID_CDROM_CD_R",           "optical_cd_r"},
        {"ID_CDROM_CD_RW",          "optical_cd_rw"},
        {"ID_CDROM_DVD",            "optical_dvd"},
        {"ID_CDROM_DVD_R",          "optical_dvd_r"},
        {"ID_CDROM_DVD_RW",         "optical_dvd_rw"},
        {"ID_CDROM_DVD_RAM",        "optical_dvd_ram"},
        {"ID_CDROM_DVD_PLUS_R",     "optical_dvd_plus_r"},
        {"ID_CDROM_DVD_PLUS_RW",    "optical_dvd_plus_rw"},
        {"ID_CDROM_DVD_PLUS_R_DL",  "optical_dvd_plus_r_dl"},
        {"ID_CDROM_DVD_PLUS_RW_DL", "optical_dvd_plus_rw_dl"},
        {"ID_CDROM_BD",             "optical_bd"},
        {"ID_CDROM_BD_R",           "optical_bd_r"},
        {"ID_CDROM_BD_RE",          "optical_bd_re"},
        {"ID_CDROM_HDDVD",          "optical_hddvd"},
        {"ID_CDROM_HDDVD_R",        "optical_hddvd_r"},
        {"ID_CDROM_HDDVD_RW",       "optical_hddvd_rw"},
        {"ID_CDROM_MO",             "optical_mo"},
        {"ID_CDROM_MRW",            "optical_mrw"},
        {"ID_CDROM_MRW_W",          "optical_mrw_w"},
        {NULL, NULL},
};

static const struct
{
        const gchar *udev_property;
        const gchar *media_name;
} media_mapping[] = {
        {"ID_DRIVE_MEDIA_FLASH",          "flash"},
        {"ID_DRIVE_MEDIA_FLASH_CF",       "flash_cf"},
        {"ID_DRIVE_MEDIA_FLASH_MS",       "flash_ms"},
        {"ID_DRIVE_MEDIA_FLASH_SM",       "flash_sm"},
        {"ID_DRIVE_MEDIA_FLASH_SD",       "flash_sd"},
        {"ID_DRIVE_MEDIA_FLASH_SDHC",     "flash_sdhc"},
        {"ID_DRIVE_MEDIA_FLASH_MMC",      "flash_mmc"},
        {"ID_DRIVE_MEDIA_FLOPPY",         "floppy"},
        {"ID_DRIVE_MEDIA_FLOPPY_ZIP",     "floppy_zip"},
        {"ID_DRIVE_MEDIA_FLOPPY_JAZ",     "floppy_jaz"},
        {"ID_CDROM_MEDIA_CD",             "optical_cd"},
        {"ID_CDROM_MEDIA_CD_R",           "optical_cd_r"},
        {"ID_CDROM_MEDIA_CD_RW",          "optical_cd_rw"},
        {"ID_CDROM_MEDIA_DVD",            "optical_dvd"},
        {"ID_CDROM_MEDIA_DVD_R",          "optical_dvd_r"},
        {"ID_CDROM_MEDIA_DVD_RW",         "optical_dvd_rw"},
        {"ID_CDROM_MEDIA_DVD_RAM",        "optical_dvd_ram"},
        {"ID_CDROM_MEDIA_DVD_PLUS_R",     "optical_dvd_plus_r"},
        {"ID_CDROM_MEDIA_DVD_PLUS_RW",    "optical_dvd_plus_rw"},
        {"ID_CDROM_MEDIA_DVD_PLUS_R_DL",  "optical_dvd_plus_r_dl"},
        {"ID_CDROM_MEDIA_DVD_PLUS_RW_DL", "optical_dvd_plus_rw_dl"},
        {"ID_CDROM_MEDIA_BD",             "optical_bd"},
        {"ID_CDROM_MEDIA_BD_R",           "optical_bd_r"},
        {"ID_CDROM_MEDIA_BD_RE",          "optical_bd_re"},
        {"ID_CDROM_MEDIA_HDDVD",          "optical_hddvd"},
        {"ID_CDROM_MEDIA_HDDVD_R",        "optical_hddvd_r"},
        {"ID_CDROM_MEDIA_HDDVD_RW",       "optical_hddvd_rw"},
        {"ID_CDROM_MEDIA_MO",             "optical_mo"},
        {"ID_CDROM_MEDIA_MRW",            "optical_mrw"},
        {"ID_CDROM_MEDIA_MRW_W",          "optical_mrw_w"},
        {NULL, NULL},
};

/* update drive_* properties */
static gboolean
update_info_drive (DevkitDisksDevice *device)
{
        GPtrArray *media_compat_array;
        const gchar *media_in_drive;
        gboolean drive_is_ejectable;
        gboolean drive_can_detach;
        gchar *decoded_string;
        guint n;

        if (g_udev_device_has_property (device->priv->d, "ID_VENDOR_ENC")) {
                decoded_string = decode_udev_encoded_string (g_udev_device_get_property (device->priv->d, "ID_VENDOR_ENC"));
                g_strstrip (decoded_string);
                devkit_disks_device_set_drive_vendor (device, decoded_string);
                g_free (decoded_string);
        } else if (g_udev_device_has_property (device->priv->d, "ID_VENDOR")) {
                devkit_disks_device_set_drive_vendor (device, g_udev_device_get_property (device->priv->d, "ID_VENDOR"));
        }

        if (g_udev_device_has_property (device->priv->d, "ID_MODEL_ENC")) {
                decoded_string = decode_udev_encoded_string (g_udev_device_get_property (device->priv->d, "ID_MODEL_ENC"));
                g_strstrip (decoded_string);
                devkit_disks_device_set_drive_model (device, decoded_string);
                g_free (decoded_string);
        } else if (g_udev_device_has_property (device->priv->d, "ID_MODEL")) {
                devkit_disks_device_set_drive_model (device, g_udev_device_get_property (device->priv->d, "ID_MODEL"));
        }

        if (g_udev_device_has_property (device->priv->d, "ID_REVISION"))
                devkit_disks_device_set_drive_revision (device, g_udev_device_get_property (device->priv->d, "ID_REVISION"));
        if (g_udev_device_has_property (device->priv->d, "ID_SERIAL_SHORT"))
                devkit_disks_device_set_drive_serial (device, g_udev_device_get_property (device->priv->d, "ID_SERIAL_SHORT"));

        /* pick up some things (vendor, model, connection_interface, connection_speed)
         * not (yet) exported by udev helpers
         */
        update_drive_properties_from_sysfs (device);

        if (g_udev_device_has_property (device->priv->d, "ID_DRIVE_EJECTABLE")) {
                drive_is_ejectable = g_udev_device_get_property_as_boolean (device->priv->d, "ID_DRIVE_EJECTABLE");
        } else {
                drive_is_ejectable = FALSE;
                drive_is_ejectable |= g_udev_device_has_property (device->priv->d, "ID_CDROM");
                drive_is_ejectable |= g_udev_device_has_property (device->priv->d, "ID_DRIVE_FLOPPY_ZIP");
                drive_is_ejectable |= g_udev_device_has_property (device->priv->d, "ID_DRIVE_FLOPPY_JAZ");
        }
        devkit_disks_device_set_drive_is_media_ejectable (device, drive_is_ejectable);

        media_compat_array = g_ptr_array_new ();
        for (n = 0; drive_media_mapping[n].udev_property != NULL; n++) {
                if (!g_udev_device_has_property (device->priv->d, drive_media_mapping[n].udev_property))
                        continue;

                g_ptr_array_add (media_compat_array, (gpointer) drive_media_mapping[n].media_name);
        }
        /* special handling for SDIO since we don't yet have a sdio_id helper in udev to set properties */
        if (g_strcmp0 (device->priv->drive_connection_interface, "sdio") == 0) {
                gchar *type;

                type = sysfs_get_string (device->priv->native_path, "../../type");
                g_strstrip (type);
                if (g_strcmp0 (type, "MMC") == 0) {
                        g_ptr_array_add (media_compat_array, "flash_mmc");
                } else if (g_strcmp0 (type, "SD") == 0) {
                        g_ptr_array_add (media_compat_array, "flash_sd");
                } else if (g_strcmp0 (type, "SDHC") == 0) {
                        g_ptr_array_add (media_compat_array, "flash_sdhc");
                }
                g_free (type);
        }
        g_ptr_array_sort (media_compat_array, (GCompareFunc) ptr_str_array_compare);
        g_ptr_array_add (media_compat_array, NULL);
        devkit_disks_device_set_drive_media_compatibility (device, (GStrv) media_compat_array->pdata);

        media_in_drive = NULL;

        if (device->priv->device_is_media_available) {
                for (n = 0; media_mapping[n].udev_property != NULL; n++) {
                        if (!g_udev_device_has_property (device->priv->d, media_mapping[n].udev_property))
                                continue;

                        media_in_drive = drive_media_mapping[n].media_name;
                        break;
                }
                /* If the media isn't set (from e.g. udev rules), just pick the first one in media_compat - note
                 * that this may be NULL (if we don't know what media is compatible with the drive) which is OK.
                 */
                if (media_in_drive == NULL)
                        media_in_drive = ((const gchar **) media_compat_array->pdata)[0];
        }
        devkit_disks_device_set_drive_media (device, media_in_drive);

        g_ptr_array_free (media_compat_array, TRUE);

        /* right now, we only offer to detach USB devices */
        drive_can_detach = FALSE;
        if (g_strcmp0 (device->priv->drive_connection_interface, "usb") == 0) {
                drive_can_detach = TRUE;
        }
        if (g_udev_device_has_property (device->priv->d, "ID_DRIVE_DETACHABLE")) {
                drive_can_detach = g_udev_device_get_property_as_boolean (device->priv->d, "ID_DRIVE_DETACHABLE");
        }
        devkit_disks_device_set_drive_can_detach (device, drive_can_detach);

        /* rotational is in sysfs */
        devkit_disks_device_set_drive_is_rotational (device,
                                                     g_udev_device_get_sysfs_attr_as_boolean (device->priv->d,
                                                                                              "queue/rotational"));

        return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/* update drive_can_spindown property */
static gboolean
update_info_drive_can_spindown (DevkitDisksDevice *device)
{
        gboolean drive_can_spindown;

        /* Right now we only know how to spin down ATA devices (including those USB devices
         * that can do ATA SMART)
         *
         * This would probably also work for SCSI devices (since the helper is doing SCSI
         * STOP (which translated in libata to ATA's STANDBY IMMEDIATE) - but that needs
         * testing...
         */
        drive_can_spindown = FALSE;
        if (g_strcmp0 (device->priv->drive_connection_interface, "ata") == 0 ||
            device->priv->drive_ata_smart_is_available) {
                drive_can_spindown = TRUE;
        }
        if (g_udev_device_has_property (device->priv->d, "ID_DRIVE_CAN_SPINDOWN")) {
                drive_can_spindown = g_udev_device_get_property_as_boolean (device->priv->d, "ID_DRIVE_CAN_SPINDOWN");
        }
        devkit_disks_device_set_drive_can_spindown (device, drive_can_spindown);

        return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/* update device_is_optical_disc and optical_disc_* properties */
static gboolean
update_info_optical_disc (DevkitDisksDevice *device)
{
        const gchar *cdrom_disc_state;
        gint cdrom_track_count;
        gint cdrom_track_count_audio;
        gint cdrom_session_count;

        /* device_is_optical_disc and optical_disc_* */
        if (g_udev_device_has_property (device->priv->d, "ID_CDROM_MEDIA")) {
                devkit_disks_device_set_device_is_optical_disc (device, TRUE);

                cdrom_track_count = 0;
                cdrom_track_count_audio = 0;
                cdrom_session_count = 0;

                if (g_udev_device_has_property (device->priv->d, "ID_CDROM_MEDIA_TRACK_COUNT"))
                        cdrom_track_count = g_udev_device_get_property_as_int (device->priv->d, "ID_CDROM_MEDIA_TRACK_COUNT");
                if (g_udev_device_has_property (device->priv->d, "ID_CDROM_MEDIA_TRACK_COUNT_AUDIO"))
                        cdrom_track_count_audio = g_udev_device_get_property_as_int (device->priv->d, "ID_CDROM_MEDIA_TRACK_COUNT_AUDIO");
                if (g_udev_device_has_property (device->priv->d, "ID_CDROM_MEDIA_SESSION_COUNT"))
                        cdrom_session_count = g_udev_device_get_property_as_int (device->priv->d, "ID_CDROM_MEDIA_SESSION_COUNT");
                devkit_disks_device_set_optical_disc_num_tracks (device, cdrom_track_count);
                devkit_disks_device_set_optical_disc_num_audio_tracks (device, cdrom_track_count_audio);
                devkit_disks_device_set_optical_disc_num_sessions (device, cdrom_session_count);
                cdrom_disc_state = g_udev_device_get_property (device->priv->d, "ID_CDROM_MEDIA_STATE");
                devkit_disks_device_set_optical_disc_is_blank (device, g_strcmp0 (cdrom_disc_state, "blank") == 0);
                devkit_disks_device_set_optical_disc_is_appendable (device, g_strcmp0 (cdrom_disc_state, "appendable") == 0);
                devkit_disks_device_set_optical_disc_is_closed (device, g_strcmp0 (cdrom_disc_state, "complete") == 0);
        } else {
                devkit_disks_device_set_device_is_optical_disc (device, FALSE);

                devkit_disks_device_set_optical_disc_num_tracks (device, 0);
                devkit_disks_device_set_optical_disc_num_audio_tracks (device, 0);
                devkit_disks_device_set_optical_disc_num_sessions (device, 0);
                devkit_disks_device_set_optical_disc_is_blank (device, FALSE);
                devkit_disks_device_set_optical_disc_is_appendable (device, FALSE);
                devkit_disks_device_set_optical_disc_is_closed (device, FALSE);
        }

        return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/* update device_is_luks and luks_holder properties */
static gboolean
update_info_luks (DevkitDisksDevice *device)
{
        if (g_strcmp0 (device->priv->id_type, "crypto_LUKS") == 0 &&
            device->priv->holders_objpath->len == 1) {
                devkit_disks_device_set_device_is_luks (device, TRUE);
                devkit_disks_device_set_luks_holder (device, device->priv->holders_objpath->pdata[0]);
        } else {
                devkit_disks_device_set_device_is_luks (device, FALSE);
                devkit_disks_device_set_luks_holder (device, NULL);
        }

        return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/* update device_is_luks_cleartext and luks_cleartext_* properties */
static gboolean
update_info_luks_cleartext (DevkitDisksDevice *device)
{
        uid_t unlocked_by_uid;
        const gchar *dkd_dm_name;
        const gchar *dkd_dm_target_types;
        gboolean ret;

        ret = FALSE;

        dkd_dm_name = g_udev_device_get_property (device->priv->d, "DKD_DM_NAME");
        dkd_dm_target_types = g_udev_device_get_property (device->priv->d, "DKD_DM_TARGET_TYPES");
        if (dkd_dm_name != NULL && g_strcmp0 (dkd_dm_target_types, "crypt") == 0 &&
            device->priv->slaves_objpath->len == 1) {

                /* TODO: might be racing with setting is_drive earlier */
                devkit_disks_device_set_device_is_drive (device, FALSE);

                if (g_str_has_prefix (dkd_dm_name, "temporary-cryptsetup-")) {
                        /* ignore temporary devices created by /sbin/cryptsetup */
                        goto out;
                }

                devkit_disks_device_set_device_is_luks_cleartext (device, TRUE);

                devkit_disks_device_set_luks_cleartext_slave (device, ((gchar **) device->priv->slaves_objpath->pdata)[0]);

                if (luks_get_uid_from_dm_name (dkd_dm_name, &unlocked_by_uid)) {
                        devkit_disks_device_set_luks_cleartext_unlocked_by_uid (device, unlocked_by_uid);
                }

                /* TODO: export this at some point */
                devkit_disks_device_set_dm_name (device, dkd_dm_name);
        } else {
                devkit_disks_device_set_device_is_luks_cleartext (device, FALSE);
                devkit_disks_device_set_luks_cleartext_slave (device, NULL);
        }

        ret = TRUE;

 out:
        return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/* update device_is_linux_md_component and linux_md_component_* properties */
static gboolean
update_info_linux_md_component (DevkitDisksDevice *device)
{
        if (g_strcmp0 (device->priv->id_type, "linux_raid_member") == 0) {
                const gchar *md_comp_level;
                gint md_comp_num_raid_devices;
                const gchar *md_comp_uuid;
                const gchar *md_comp_home_host;
                const gchar *md_comp_name;
                const gchar *md_comp_version;
                gchar *md_name;
                gchar *s;

                devkit_disks_device_set_device_is_linux_md_component (device, TRUE);

                /* linux_md_component_holder and linux_md_component_state */
                if (device->priv->holders_objpath->len == 1) {
                        DevkitDisksDevice *holder;
                        gchar **state_tokens;

                        devkit_disks_device_set_linux_md_component_holder (device, device->priv->holders_objpath->pdata[0]);
                        state_tokens = NULL;
                        holder = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon,
                                                                                device->priv->holders_objpath->pdata[0]);
                        if (holder != NULL && holder->priv->device_is_linux_md) {
                                gchar *dev_name;
                                gchar *md_dev_path;
                                gchar *state_contents;

                                dev_name = g_path_get_basename (device->priv->native_path);
                                md_dev_path = g_strdup_printf ("%s/md/dev-%s", holder->priv->native_path, dev_name);
                                state_contents = sysfs_get_string (md_dev_path, "state");
                                g_strstrip (state_contents);
                                state_tokens = g_strsplit (state_contents, ",", 0);

                                g_free (state_contents);
                                g_free (md_dev_path);
                                g_free (dev_name);
                        }

                        devkit_disks_device_set_linux_md_component_state (device, state_tokens);
                        g_strfreev (state_tokens);

                } else {
                        /* no holder, nullify properties */
                        devkit_disks_device_set_linux_md_component_holder (device, NULL);
                        devkit_disks_device_set_linux_md_component_state (device, NULL);
                }

                md_comp_level = g_udev_device_get_property (device->priv->d, "MD_LEVEL");
                md_comp_num_raid_devices = g_udev_device_get_property_as_int (device->priv->d, "MD_DEVICES");
                md_comp_uuid = g_udev_device_get_property (device->priv->d, "MD_UUID");
                md_name = g_strdup (g_udev_device_get_property (device->priv->d, "MD_NAME"));
                s = NULL;
                if (md_name != NULL)
                        s = strstr (md_name, ":");
                if (s != NULL) {
                        *s = '\0';
                        md_comp_home_host = md_name;
                        md_comp_name = s + 1;
                } else {
                        md_comp_home_host = "";
                        md_comp_name = md_name;
                }
                md_comp_version = device->priv->id_version;

                devkit_disks_device_set_linux_md_component_level (device, md_comp_level);
                devkit_disks_device_set_linux_md_component_num_raid_devices (device, md_comp_num_raid_devices);
                devkit_disks_device_set_linux_md_component_uuid (device, md_comp_uuid);
                devkit_disks_device_set_linux_md_component_home_host (device, md_comp_home_host);
                devkit_disks_device_set_linux_md_component_name (device, md_comp_name);
                devkit_disks_device_set_linux_md_component_version (device, md_comp_version);

                g_free (md_name);
        } else {
                devkit_disks_device_set_device_is_linux_md_component (device, FALSE);
                devkit_disks_device_set_linux_md_component_level (device, NULL);
                devkit_disks_device_set_linux_md_component_num_raid_devices (device, 0);
                devkit_disks_device_set_linux_md_component_uuid (device, NULL);
                devkit_disks_device_set_linux_md_component_home_host (device, NULL);
                devkit_disks_device_set_linux_md_component_name (device, NULL);
                devkit_disks_device_set_linux_md_component_version (device, NULL);
                devkit_disks_device_set_linux_md_component_holder (device, NULL);
                devkit_disks_device_set_linux_md_component_state (device, NULL);
        }

        return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/* update device_is_linux_md and linux_md_* properties */
static gboolean
update_info_linux_md (DevkitDisksDevice *device)
{
        gboolean ret;
        guint n;
        gchar *s;
        gchar *p;

        ret = FALSE;

        if (sysfs_file_exists (device->priv->native_path, "md")) {
                gchar *uuid;
                gint num_raid_devices;
                gchar *raid_level;
                gchar *array_state;
                DevkitDisksDevice *slave;
                GPtrArray *md_slaves;
                const gchar *md_name;
                const gchar *md_home_host;

                devkit_disks_device_set_device_is_linux_md (device, TRUE);

                /* figure out if the array is active */
                array_state = sysfs_get_string (device->priv->native_path, "md/array_state");
                if (array_state == NULL) {
                        g_print ("**** NOTE: Linux MD array %s has no array_state file'; removing\n", device->priv->native_path);
                        goto out;
                }
                g_strstrip (array_state);

                /* ignore clear arrays since these have no devices, no size, no level */
                if (strcmp (array_state, "clear") == 0) {
                        g_print ("**** NOTE: Linux MD array %s is 'clear'; removing\n", device->priv->native_path);
                        g_free (array_state);
                        goto out;
                }

                devkit_disks_device_set_linux_md_state (device, array_state);
                g_free (array_state);

                /* find a slave from the array */
                slave = NULL;
                for (n = 0; n < device->priv->slaves_objpath->len; n++) {
                        const gchar *slave_objpath;

                        slave_objpath = device->priv->slaves_objpath->pdata[n];
                        slave = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, slave_objpath);
                        if (slave != NULL)
                                break;
                }

                uuid = g_strdup (g_udev_device_get_property (device->priv->d, "MD_UUID"));
                num_raid_devices = sysfs_get_int (device->priv->native_path, "md/raid_disks");
                raid_level = g_strstrip (sysfs_get_string (device->priv->native_path, "md/level"));

                if (slave != NULL) {
                        /* if the UUID isn't set by the udev rules (array may be inactive) get it from a slave */
                        if (uuid == NULL || strlen (uuid) == 0) {
                                g_free (uuid);
                                uuid = g_strdup (slave->priv->linux_md_component_uuid);
                        }

                        /* ditto for raid level */
                        if (raid_level == NULL || strlen (raid_level) == 0) {
                                g_free (raid_level);
                                raid_level = g_strdup (slave->priv->linux_md_component_level);
                        }

                        /* and num_raid_devices too */
                        if (device->priv->linux_md_num_raid_devices == 0) {
                                num_raid_devices = slave->priv->linux_md_component_num_raid_devices;
                        }
                }

                devkit_disks_device_set_linux_md_uuid (device, uuid);
                devkit_disks_device_set_linux_md_num_raid_devices (device, num_raid_devices);
                devkit_disks_device_set_linux_md_level (device, raid_level);
                g_free (raid_level);
                g_free (uuid);

                /* infer the array name and homehost */
                p = g_strdup (g_udev_device_get_property (device->priv->d, "MD_NAME"));
                s = NULL;
                if (p != NULL)
                        s = strstr (p, ":");
                if (s != NULL) {
                        *s = '\0';
                        md_home_host = p;
                        md_name = s + 1;
                } else {
                        md_home_host = "";
                        md_name = p;
                }
                devkit_disks_device_set_linux_md_home_host (device, md_home_host);
                devkit_disks_device_set_linux_md_name (device, md_name);
                g_free (p);

                s = g_strstrip (sysfs_get_string (device->priv->native_path, "md/metadata_version"));
                devkit_disks_device_set_linux_md_version (device, s);
                g_free (s);

                /* Go through all block slaves and build up the linux_md_slaves property
                 *
                 * Also update the slaves since the slave state may have changed.
                 */
                md_slaves = g_ptr_array_new ();
                for (n = 0; n < device->priv->slaves_objpath->len; n++) {
                        DevkitDisksDevice *slave_device;
                        const gchar *slave_objpath;

                        slave_objpath = device->priv->slaves_objpath->pdata[n];
                        g_ptr_array_add (md_slaves, (gpointer) slave_objpath);
                        slave_device = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, slave_objpath);
                        if (slave_device != NULL) {
                                update_info (slave_device);
                        }
                }
                g_ptr_array_sort (md_slaves, (GCompareFunc) ptr_str_array_compare);
                g_ptr_array_add (md_slaves, NULL);
                devkit_disks_device_set_linux_md_slaves (device, (GStrv) md_slaves->pdata);
                g_ptr_array_free (md_slaves, TRUE);

                /* TODO: may race */
                devkit_disks_device_set_drive_vendor (device, "Linux");
                if (device->priv->linux_md_level != NULL)
                        s = g_strdup_printf ("Software RAID %s", device->priv->linux_md_level);
                else
                        s = g_strdup_printf ("Software RAID");
                devkit_disks_device_set_drive_model (device, s);
                g_free (s);
                devkit_disks_device_set_drive_revision (device, device->priv->linux_md_version);
                devkit_disks_device_set_drive_connection_interface (device, "virtual");

                /* RAID-0 can never resync or run degraded */
                if (g_strcmp0 (device->priv->linux_md_level, "raid0") == 0 ||
                    g_strcmp0 (device->priv->linux_md_level, "linear") == 0) {
                        devkit_disks_device_set_linux_md_sync_action (device, "idle");
                        devkit_disks_device_set_linux_md_is_degraded (device, FALSE);
                } else {
                        gchar *degraded_file;
                        gint num_degraded_devices;

                        degraded_file = sysfs_get_string (device->priv->native_path, "md/degraded");
                        if (degraded_file == NULL) {
                                num_degraded_devices = 0;
                        } else {
                                num_degraded_devices = strtol (degraded_file, NULL, 0);
                        }
                        g_free (degraded_file);

                        devkit_disks_device_set_linux_md_is_degraded (device, (num_degraded_devices > 0));

                        s = g_strstrip (sysfs_get_string (device->priv->native_path, "md/sync_action"));
                        devkit_disks_device_set_linux_md_sync_action (device, s);
                        g_free (s);

                        if (device->priv->linux_md_sync_action == NULL ||
                            strlen (device->priv->linux_md_sync_action) == 0) {
                                devkit_disks_device_set_linux_md_sync_action (device, "idle");
                        }

                        /* if not idle; update percentage and speed */
                        if (g_strcmp0 (device->priv->linux_md_sync_action, "idle") != 0) {
                                char *s;
                                guint64 done;
                                guint64 remaining;

                                s = g_strstrip (sysfs_get_string (device->priv->native_path, "md/sync_completed"));
                                if (sscanf (s, "%" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT "", &done, &remaining) == 2) {
                                        devkit_disks_device_set_linux_md_sync_percentage (device,
                                                                     100.0 * ((double) done) / ((double) remaining));
                                } else {
                                        g_warning ("cannot parse md/sync_completed for %s: '%s'",
                                                   device->priv->native_path,
                                                   s);
                                }
                                g_free (s);

                                devkit_disks_device_set_linux_md_sync_speed (device,
                                                        1000L * sysfs_get_uint64 (device->priv->native_path, "md/sync_speed"));

                                /* Since the kernel doesn't emit uevents while the job is pending, set up
                                 * a timeout for every two seconds to synthesize the change event so we can
                                 * refresh the completed/speed properties.
                                 */
                                if (device->priv->linux_md_poll_timeout_id == 0) {
                                        device->priv->linux_md_poll_timeout_id =
                                                g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                                                            2,
                                                                            poll_syncing_md_device,
                                                                            g_object_ref (device),
                                                                            g_object_unref);
                                }
                        } else {
                                devkit_disks_device_set_linux_md_sync_percentage (device, 0.0);
                                devkit_disks_device_set_linux_md_sync_speed (device, 0);
                        }
                }

        } else {
                devkit_disks_device_set_device_is_linux_md (device, FALSE);
                devkit_disks_device_set_linux_md_state (device, NULL);
                devkit_disks_device_set_linux_md_level (device, NULL);
                devkit_disks_device_set_linux_md_num_raid_devices (device, 0);
                devkit_disks_device_set_linux_md_uuid (device, NULL);
                devkit_disks_device_set_linux_md_home_host (device, NULL);
                devkit_disks_device_set_linux_md_name (device, NULL);
                devkit_disks_device_set_linux_md_version (device, NULL);
                devkit_disks_device_set_linux_md_slaves (device, NULL);
                devkit_disks_device_set_linux_md_is_degraded (device, FALSE);
                devkit_disks_device_set_linux_md_sync_action (device, NULL);
                devkit_disks_device_set_linux_md_sync_percentage (device, 0.0);
                devkit_disks_device_set_linux_md_sync_speed (device, 0);
        }

        ret = TRUE;

 out:
        return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/* update drive_ata_smart_* properties */
static gboolean
update_info_drive_ata_smart (DevkitDisksDevice *device)
{
        gboolean ata_smart_is_available;

        ata_smart_is_available = FALSE;
        if (device->priv->device_is_drive &&
            g_udev_device_has_property (device->priv->d, "DKD_ATA_SMART_IS_AVAILABLE"))
                ata_smart_is_available = g_udev_device_get_property_as_boolean (device->priv->d, "DKD_ATA_SMART_IS_AVAILABLE");

        devkit_disks_device_set_drive_ata_smart_is_available (device, ata_smart_is_available);

        /* NOTE: we don't collect ATA SMART data here, we only set whether the device is ATA SMART capable;
         *       collecting data is done in separate routines, see the
         *       devkit_disks_device_drive_ata_smart_refresh_data() function for details.
         */

        return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/* device_is_system_internal */
static gboolean
update_info_is_system_internal (DevkitDisksDevice *device)
{
        gboolean is_system_internal;

        /* TODO: make it possible to override this property from a udev property.
         */

        /* start out by assuming the device is system internal, then adjust depending on what kind of
         * device we are dealing with
         */
        is_system_internal = TRUE;

        /* A Linux MD device is system internal if, and only if
         *
         * - a single component is system internal
         * - there are no components
         */
        if (device->priv->device_is_linux_md) {
                is_system_internal = FALSE;

                if (device->priv->slaves_objpath->len == 0) {
                        is_system_internal = TRUE;
                } else {
                        guint n;

                        for (n = 0; n < device->priv->slaves_objpath->len; n++) {
                                const gchar *slave_objpath;
                                DevkitDisksDevice *slave;

                                slave_objpath = device->priv->slaves_objpath->pdata[n];
                                slave = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, slave_objpath);
                                if (slave == NULL)
                                        continue;

                                if (slave->priv->device_is_system_internal) {
                                        is_system_internal = TRUE;
                                        break;
                                }
                        }
                }

                goto determined;
        }

        /* a partition is system internal only if the drive it belongs to is system internal */
        if (device->priv->device_is_partition) {
                DevkitDisksDevice *enclosing_device;

                enclosing_device = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, device->priv->partition_slave);
                if (enclosing_device != NULL) {
                        is_system_internal = enclosing_device->priv->device_is_system_internal;
                } else {
                        is_system_internal = TRUE;
                }

                goto determined;
        }

        /* a LUKS cleartext device is system internal only if the underlying crypto-text
         * device is system internal
         */
        if (device->priv->device_is_luks_cleartext) {
                DevkitDisksDevice *enclosing_device;
                enclosing_device = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, device->priv->luks_cleartext_slave);
                if (enclosing_device != NULL) {
                        is_system_internal = enclosing_device->priv->device_is_system_internal;
                } else {
                        is_system_internal = TRUE;
                }

                goto determined;
        }

        /* devices with removable media are never system internal */
        if (device->priv->device_is_removable) {
                is_system_internal = FALSE;
                goto determined;
        }

        /* devices on certain buses are never system internal */
        if (device->priv->device_is_drive && device->priv->drive_connection_interface != NULL) {

                if (strcmp (device->priv->drive_connection_interface, "ata_serial_esata") == 0 ||
                    strcmp (device->priv->drive_connection_interface, "sdio") == 0 ||
                    strcmp (device->priv->drive_connection_interface, "usb") == 0 ||
                    strcmp (device->priv->drive_connection_interface, "firewire") == 0) {
                        is_system_internal = FALSE;
                } else {
                        is_system_internal = TRUE;
                }
                goto determined;
        }

 determined:
        devkit_disks_device_set_device_is_system_internal (device, is_system_internal);

        return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/* device_is_mounted, device_mount, device_mounted_by_uid */
static gboolean
update_info_mount_state (DevkitDisksDevice *device)
{
        DevkitDisksMountMonitor *monitor;
        GList *mounts;
        gboolean was_mounted;

        mounts = NULL;

        /* defer setting the mount point until FilesystemMount returns and
         * the mounts file is written
         */
        if (device->priv->job_in_progress && g_strcmp0 (device->priv->job_id, "FilesystemMount") == 0)
                goto out;

        monitor = devkit_disks_daemon_local_get_mount_monitor (device->priv->daemon);

        mounts = devkit_disks_mount_monitor_get_mounts_for_dev (monitor, device->priv->dev);

        was_mounted = device->priv->device_is_mounted;

        if (mounts != NULL) {
                GList *l;
                guint n;
                gchar **mount_paths;

                mount_paths = g_new0 (gchar *, g_list_length (mounts) + 1);
                for (l = mounts, n = 0; l != NULL; l = l->next, n++) {
                        mount_paths[n] = g_strdup (devkit_disks_mount_get_mount_path (DEVKIT_DISKS_MOUNT (l->data)));
                }

                devkit_disks_device_set_device_is_mounted (device, TRUE);
                devkit_disks_device_set_device_mount_paths (device, mount_paths);
                if (!was_mounted) {
                        uid_t mounted_by_uid;

                        if (!devkit_disks_mount_file_has_device (device->priv->device_file, &mounted_by_uid, NULL))
                                mounted_by_uid = 0;
                        devkit_disks_device_set_device_mounted_by_uid (device, mounted_by_uid);
                }

                g_strfreev (mount_paths);

        } else {
                gboolean remove_dir_on_unmount;
                gchar *old_mount_path;

                old_mount_path = NULL;
                if (device->priv->device_mount_paths->len > 0)
                        old_mount_path = g_strdup (((gchar **) device->priv->device_mount_paths->pdata)[0]);

                devkit_disks_device_set_device_is_mounted (device, FALSE);
                devkit_disks_device_set_device_mount_paths (device, NULL);
                devkit_disks_device_set_device_mounted_by_uid (device, 0);

                /* clean up stale mount directory */
                remove_dir_on_unmount = FALSE;
                if (was_mounted && devkit_disks_mount_file_has_device (device->priv->device_file,
                                                                       NULL,
                                                                       &remove_dir_on_unmount)) {
                        devkit_disks_mount_file_remove (device->priv->device_file, old_mount_path);
                        if (remove_dir_on_unmount) {
                                if (g_rmdir (old_mount_path) != 0) {
                                        g_warning ("Error removing dir '%s' on unmount: %m", old_mount_path);
                                }
                        }
                }

                g_free (old_mount_path);

        }

 out:
        g_list_foreach (mounts, (GFunc) g_object_unref, NULL);
        g_list_free (mounts);

        return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/* device_is_media_change_detected, device_is_media_change_detection_* properties */
static gboolean
update_info_media_detection (DevkitDisksDevice *device)
{
        gboolean detected;
        gboolean polling;
        gboolean inhibitable;
        gboolean inhibited;

        detected = FALSE;
        polling = FALSE;
        inhibitable = FALSE;
        inhibited = FALSE;

        if (device->priv->device_is_removable) {
                guint64 evt_media_change;
                GUdevDevice *parent;

                evt_media_change = sysfs_get_uint64 (device->priv->native_path, "../../evt_media_change");
                if (evt_media_change & 1) {
                        /* SATA AN capabable drive */

                        polling = FALSE;
                        detected = TRUE;
                        goto determined;
                }

                parent = g_udev_device_get_parent_with_subsystem (device->priv->d,
                                                                  "platform",
                                                                  NULL);
                if (parent != NULL) {
                        /* never poll PC floppy drives, they are noisy (fdo #22149) */
                        if (g_str_has_prefix (g_udev_device_get_name (parent), "floppy.")) {
                                g_object_unref (parent);
                                goto determined;
                        }
                        g_object_unref (parent);
                }

                /* assume the device needs polling */
                polling = TRUE;
                inhibitable = TRUE;

                if (device->priv->polling_inhibitors != NULL ||
                    devkit_disks_daemon_local_has_polling_inhibitors (device->priv->daemon)) {

                        detected = FALSE;
                        inhibited = TRUE;
                } else {
                        detected = TRUE;
                        inhibited = FALSE;
                }
        }

 determined:
        devkit_disks_device_set_device_is_media_change_detected (device, detected);
        devkit_disks_device_set_device_is_media_change_detection_polling (device, polling);
        devkit_disks_device_set_device_is_media_change_detection_inhibitable (device, inhibitable);
        devkit_disks_device_set_device_is_media_change_detection_inhibited (device, inhibited);

        return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * update_info:
 * @device: the device
 *
 * Update information about the device.
 *
 * If one or more properties changed, the changes are scheduled to be emitted. Use
 * drain_pending_changes() to force emitting the pending changes (which is useful
 * before returning the result of an operation).
 *
 * Returns: #TRUE to keep (or add) the device; #FALSE to ignore (or remove) the device
 **/
static gboolean
update_info (DevkitDisksDevice *device)
{
        guint64 start, size;
        char *s;
        guint n;
        gboolean ret;
        char *path;
        GDir *dir;
        const char *name;
        GList *l;
        GList *old_slaves_objpath;
        GList *old_holders_objpath;
        GList *cur_slaves_objpath;
        GList *cur_holders_objpath;
        GList *added_objpath;
        GList *removed_objpath;
        GPtrArray *symlinks_by_id;
        GPtrArray *symlinks_by_path;
        GPtrArray *slaves;
        GPtrArray *holders;
        gint major;
        gint minor;
        gboolean media_available;

        ret = FALSE;

        g_print ("**** UPDATING %s\n", device->priv->native_path);

        /* need the slaves/holders to synthesize 'change' events if a device goes away (since the kernel
         * doesn't do generate these)
         */
        old_slaves_objpath = dup_list_from_ptrarray (device->priv->slaves_objpath);
        old_holders_objpath = dup_list_from_ptrarray (device->priv->holders_objpath);

        /* drive identification */
        if (sysfs_file_exists (device->priv->native_path, "range")) {
                devkit_disks_device_set_device_is_drive (device, TRUE);
        } else {
                devkit_disks_device_set_device_is_drive (device, FALSE);
        }

        if (!g_udev_device_has_property (device->priv->d, "MAJOR") ||
            !g_udev_device_has_property (device->priv->d, "MINOR")) {
		g_warning ("No major/minor for %s", device->priv->native_path);
                goto out;
        }

        major = g_udev_device_get_property_as_int (device->priv->d, "MAJOR");
        minor = g_udev_device_get_property_as_int (device->priv->d, "MINOR");
        device->priv->dev = makedev (major, minor);

        devkit_disks_device_set_device_file (device, g_udev_device_get_device_file (device->priv->d));
        if (device->priv->device_file == NULL) {
		g_warning ("No device file for %s", device->priv->native_path);
                goto out;
        }

        const char * const * symlinks;
        symlinks = g_udev_device_get_device_file_symlinks (device->priv->d);
        symlinks_by_id = g_ptr_array_new ();
        symlinks_by_path = g_ptr_array_new ();
        for (n = 0; symlinks[n] != NULL; n++) {
                if (g_str_has_prefix (symlinks[n], "/dev/disk/by-id/") ||
                    g_str_has_prefix (symlinks[n], "/dev/disk/by-uuid/")) {
                        g_ptr_array_add (symlinks_by_id, (gpointer) symlinks[n]);
                } else if (g_str_has_prefix (symlinks[n], "/dev/disk/by-path/")) {
                        g_ptr_array_add (symlinks_by_path, (gpointer) symlinks[n]);
                }
        }
        g_ptr_array_sort (symlinks_by_id, (GCompareFunc) ptr_str_array_compare);
        g_ptr_array_sort (symlinks_by_path, (GCompareFunc) ptr_str_array_compare);
        g_ptr_array_add (symlinks_by_id, NULL);
        g_ptr_array_add (symlinks_by_path, NULL);
        devkit_disks_device_set_device_file_by_id (device, (GStrv) symlinks_by_id->pdata);
        devkit_disks_device_set_device_file_by_path (device, (GStrv) symlinks_by_path->pdata);
        g_ptr_array_free (symlinks_by_id, TRUE);
        g_ptr_array_free (symlinks_by_path, TRUE);

        devkit_disks_device_set_device_is_removable (device, (sysfs_get_int (device->priv->native_path, "removable") != 0));

        /* device_is_media_available and device_media_detection_time property */
        if (device->priv->device_is_removable) {
                media_available = FALSE;
                if (g_udev_device_has_property (device->priv->d, "DKD_MEDIA_AVAILABLE")) {
                        media_available = g_udev_device_get_property_as_boolean (device->priv->d, "DKD_MEDIA_AVAILABLE");
                } else {
                        if (g_udev_device_has_property (device->priv->d, "ID_CDROM_MEDIA_STATE")) {
                                media_available = TRUE;
                        } else {
                                media_available = FALSE;
                        }
                }
        } else {
                media_available = TRUE;
        }
        devkit_disks_device_set_device_is_media_available (device, media_available);
        if (media_available) {
                if (device->priv->device_media_detection_time == 0)
                        devkit_disks_device_set_device_media_detection_time (device, (guint64) time (NULL));
        } else {
                devkit_disks_device_set_device_media_detection_time (device, 0);
         }

        /* device_size, device_block_size and device_is_read_only properties */
        if (device->priv->device_is_media_available) {
                guint64 block_size;

                devkit_disks_device_set_device_size (device, sysfs_get_uint64 (device->priv->native_path, "size") * ((guint64) 512));
                devkit_disks_device_set_device_is_read_only (device, (sysfs_get_int (device->priv->native_path, "ro") != 0));
                /* This is not available on all devices so fall back to 512 if unavailable.
                 *
                 * Another way to get this information is the BLKSSZGET ioctl but we don't want
                 * to open the device. Ideally vol_id would export it.
                 */
                block_size = sysfs_get_uint64 (device->priv->native_path, "queue/hw_sector_size");
                if (block_size == 0)
                        block_size = 512;
                devkit_disks_device_set_device_block_size (device, block_size);

        } else {
                devkit_disks_device_set_device_size (device, 0);
                devkit_disks_device_set_device_block_size (device, 0);
                devkit_disks_device_set_device_is_read_only (device, FALSE);
        }

        /* figure out if we're a partition and, if so, who our slave is */
        if (sysfs_file_exists (device->priv->native_path, "start")) {

                /* we're partitioned by the kernel */
                devkit_disks_device_set_device_is_partition (device, TRUE);
                start = sysfs_get_uint64 (device->priv->native_path, "start");
                size = sysfs_get_uint64 (device->priv->native_path, "size");
                devkit_disks_device_set_partition_offset (device, start * 512); /* device->priv->device_block_size; */
                devkit_disks_device_set_partition_size (device, size * 512); /* device->priv->device_block_size; */

                s = device->priv->native_path;
                for (n = strlen (s) - 1; n >= 0 && g_ascii_isdigit (s[n]); n--)
                        ;
                devkit_disks_device_set_partition_number (device, strtol (s + n + 1, NULL, 0));

                s = g_strdup (device->priv->native_path);
                for (n = strlen (s) - 1; n >= 0 && s[n] != '/'; n--)
                        s[n] = '\0';
                s[n] = '\0';
                devkit_disks_device_set_partition_slave (device, compute_object_path (s));
                g_free (s);
        } else {
                /* TODO: handle partitions created by kpartx / dm-linear */
        }

        /* Figure out if we are a partition table - we don't want to rely on devkit-disks-part-id
         * for this; it might not detect all partition table formats that the kernel supports.
         *
         * The kernel guarantees that all childs are created before the uevent for the parent
         * is created. So if we have childs, we must be a partition table.
         *
         * To detect a child we check for the existance of a subdir that has the parents
         * name as a prefix (e.g. for parent sda then sda1, sda2, sdap1 etc. will work).
         */
        s = g_path_get_basename (device->priv->native_path);
        if ((dir = g_dir_open (device->priv->native_path, 0, NULL)) != NULL) {
                guint partition_count;
                partition_count = 0;
                while ((name = g_dir_read_name (dir)) != NULL) {
                        if (g_str_has_prefix (name, s)) {
                                partition_count++;
                        }
                }
                g_dir_close (dir);
                devkit_disks_device_set_partition_table_count (device, partition_count);
                devkit_disks_device_set_device_is_partition_table (device, (partition_count > 0));
        }
        g_free (s);


        /* Maintain (non-exported) properties holders and slaves for the holders resp. slaves
         * directories in sysfs. The entries in these arrays are object paths - we ignore
         * an entry unless it corresponds to an device in our local database.
         */
        path = g_build_filename (device->priv->native_path, "slaves", NULL);
        slaves = g_ptr_array_new ();
        if((dir = g_dir_open (path, 0, NULL)) != NULL) {
                while ((name = g_dir_read_name (dir)) != NULL) {
                        DevkitDisksDevice *device2;

                        s = compute_object_path (name);

                        device2 = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, s);
                        if (device2 != NULL) {
                                //g_debug ("%s has slave %s", device->priv->object_path, s);
                                g_ptr_array_add (slaves, s);
                        } else {
                                //g_debug ("%s has non-existant slave %s", device->priv->object_path, s);
                                g_free (s);
                        }
                }
                g_dir_close (dir);
        }
        g_free (path);
        g_ptr_array_sort (slaves, (GCompareFunc) ptr_str_array_compare);
        g_ptr_array_add (slaves, NULL);
        devkit_disks_device_set_slaves_objpath (device, (GStrv) slaves->pdata);
        g_ptr_array_foreach (slaves, (GFunc) g_free, NULL);
        g_ptr_array_free (slaves, TRUE);

        path = g_build_filename (device->priv->native_path, "holders", NULL);
        holders = g_ptr_array_new ();
        if((dir = g_dir_open (path, 0, NULL)) != NULL) {
                while ((name = g_dir_read_name (dir)) != NULL) {
                        DevkitDisksDevice *device2;

                        s = compute_object_path (name);
                        device2 = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, s);
                        if (device2 != NULL) {
                                //g_debug ("%s has holder %s", device->priv->object_path, s);
                                g_ptr_array_add (holders, s);
                        } else {
                                //g_debug ("%s has non-existant holder %s", device->priv->object_path, s);
                                g_free (s);
                        }
                }
                g_dir_close (dir);
        }
        g_free (path);
        g_ptr_array_sort (holders, (GCompareFunc) ptr_str_array_compare);
        g_ptr_array_add (holders, NULL);
        devkit_disks_device_set_holders_objpath (device, (GStrv) holders->pdata);
        g_ptr_array_foreach (holders, (GFunc) g_free, NULL);
        g_ptr_array_free (holders, TRUE);

        /* ------------------------------------- */
        /* Now set all properties from udev data */
        /* ------------------------------------- */

        /* at this point we have
         *
         *  - device_file
         *  - device_file_by_id
         *  - device_file_by_path
         *  - device_size
         *  - device_block_size
         *  - device_is_removable
         *  - device_is_read_only
         *  - device_is_drive
         *  - device_is_media_available
         *  - device_is_partition
         *  - device_is_partition_table
         *  - slaves_objpath
         *  - holders_objpath
         *
         *  - partition_number
         *  - partition_slave
         *
         */

        /* device_presentation_hide, device_presentation_name and device_presentation_icon_name properties */
        if (!update_info_presentation (device))
                goto out;

        /* id_* properties */
        if (!update_info_id (device))
                goto out;

        /* partition_table_* properties */
        if (!update_info_partition_table (device))
                goto out;

        /* partition_* properties */
        if (!update_info_partition (device))
                goto out;

        /* drive_* properties */
        if (!update_info_drive (device))
                goto out;

        /* device_is_optical_disc and optical_disc_* properties */
        if (!update_info_optical_disc (device))
                goto out;

        /* device_is_luks and luks_holder */
        if (!update_info_luks (device))
                goto out;

        /* device_is_luks_cleartext and luks_cleartext_* properties */
        if (!update_info_luks_cleartext (device))
                goto out;

        /* device_is_linux_md_component and linux_md_component_* properties */
        if (!update_info_linux_md_component (device))
                goto out;

        /* device_is_linux_md and linux_md_* properties */
        if (!update_info_linux_md (device))
                goto out;

        /* drive_ata_smart_* properties */
        if (!update_info_drive_ata_smart (device))
                goto out;

        /* drive_can_spindown property */
        if (!update_info_drive_can_spindown (device))
                goto out;

        /* device_is_system_internal property */
        if (!update_info_is_system_internal (device))
                goto out;

        /* device_is_mounted, device_mount, device_mounted_by_uid */
        if (!update_info_mount_state (device))
                goto out;

        /* device_is_media_change_detected, device_is_media_change_detection_* properties */
        if (!update_info_media_detection (device))
                goto out;

        ret = TRUE;

out:

        /* Now check if holders/ or slaves/ has changed since last update. We compute
         * the delta and do update_info() on each holder/slave that has been
         * added/removed.
         *
         * Note that this won't trigger an endless loop since we look at the diffs.
         *
         * We have to do this because the kernel doesn't generate any 'change' event
         * when slaves/ or holders/ change. This is unfortunate because we *need* such
         * a change event to update properties devices (for example: luks_holder).
         */

        cur_slaves_objpath = dup_list_from_ptrarray (device->priv->slaves_objpath);
        cur_holders_objpath = dup_list_from_ptrarray (device->priv->holders_objpath);

        old_slaves_objpath  = g_list_sort (old_slaves_objpath, (GCompareFunc) g_strcmp0);
        old_holders_objpath = g_list_sort (old_holders_objpath, (GCompareFunc) g_strcmp0);
        cur_slaves_objpath  = g_list_sort (cur_slaves_objpath, (GCompareFunc) g_strcmp0);
        cur_holders_objpath = g_list_sort (cur_holders_objpath, (GCompareFunc) g_strcmp0);

        diff_sorted_lists (old_slaves_objpath, cur_slaves_objpath,
                           (GCompareFunc) g_strcmp0,
                           &added_objpath, &removed_objpath);
        for (l = added_objpath; l != NULL; l = l->next) {
                const gchar *objpath2 = l->data;
                DevkitDisksDevice *device2;

                //g_debug ("### %s added slave %s", device->priv->object_path, objpath2);
                device2 = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, objpath2);
                if (device2 != NULL) {
                        update_info (device2);
                } else {
                        g_print ("**** NOTE: %s added non-existant slave %s\n", device->priv->object_path, objpath2);
                }
        }
        for (l = removed_objpath; l != NULL; l = l->next) {
                const gchar *objpath2 = l->data;
                DevkitDisksDevice *device2;

                //g_debug ("### %s removed slave %s", device->priv->object_path, objpath2);
                device2 = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, objpath2);
                if (device2 != NULL) {
                        update_info (device2);
                } else {
                        //g_debug ("### %s removed non-existant slave %s", device->priv->object_path, objpath2);
                }
        }
        g_list_free (added_objpath);
        g_list_free (removed_objpath);

        diff_sorted_lists (old_holders_objpath, cur_holders_objpath,
                           (GCompareFunc) g_strcmp0,
                           &added_objpath, &removed_objpath);
        for (l = added_objpath; l != NULL; l = l->next) {
                const gchar *objpath2 = l->data;
                DevkitDisksDevice *device2;

                //g_debug ("### %s added holder %s", device->priv->object_path, objpath2);
                device2 = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, objpath2);
                if (device2 != NULL) {
                        update_info (device2);
                } else {
                        g_print ("**** NOTE: %s added non-existant holder %s\n", device->priv->object_path, objpath2);
                }
        }
        for (l = removed_objpath; l != NULL; l = l->next) {
                const gchar *objpath2 = l->data;
                DevkitDisksDevice *device2;

                //g_debug ("### %s removed holder %s", device->priv->object_path, objpath2);
                device2 = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, objpath2);
                if (device2 != NULL) {
                        update_info (device2);
                } else {
                        //g_debug ("### %s removed non-existant holder %s", device->priv->object_path, objpath2);
                }
        }
        g_list_free (added_objpath);
        g_list_free (removed_objpath);

        g_list_foreach (old_slaves_objpath, (GFunc) g_free, NULL);
        g_list_free (old_slaves_objpath);
        g_list_foreach (old_holders_objpath, (GFunc) g_free, NULL);
        g_list_free (old_holders_objpath);
        g_list_foreach (cur_slaves_objpath, (GFunc) g_free, NULL);
        g_list_free (cur_slaves_objpath);
        g_list_foreach (cur_holders_objpath, (GFunc) g_free, NULL);
        g_list_free (cur_holders_objpath);

        return ret;
}

/**
 * devkit_disks_device_local_is_busy:
 * @device: A #DevkitDisksDevice.
 * @check_partitions: Whether to check if partitions is busy if @device is a partition table
 * @error: Either %NULL or a #GError to set to #DEVKIT_DISKS_ERROR_BUSY and an appropriate
 * message, e.g. "Device is busy" or "A partition on the device is busy" if the device is busy.
 *
 * Checks if @device is busy.
 *
 * Returns: %TRUE if the device or, if @check_partitions is %TRUE, a partition on the device is busy.
 */
static gboolean
devkit_disks_device_local_is_busy (DevkitDisksDevice *device,
                                   gboolean           check_partitions,
                                   GError           **error)
{
        gboolean ret;

        ret = TRUE;

        /* busy if a job is pending */
        if (device->priv->job != NULL) {
                g_set_error (error, DEVKIT_DISKS_ERROR, DEVKIT_DISKS_ERROR_BUSY,
                             "A job is pending on %s", device->priv->device_file);
                goto out;
        }

        /* or if we're mounted */
        if (device->priv->device_is_mounted) {
                g_set_error (error, DEVKIT_DISKS_ERROR, DEVKIT_DISKS_ERROR_BUSY,
                             "%s is mounted", device->priv->device_file);
                goto out;
        }

        /* or if another block device is using/holding us (e.g. if holders/ is non-empty in sysfs) */
        if (device->priv->holders_objpath->len > 0) {
                g_set_error (error, DEVKIT_DISKS_ERROR, DEVKIT_DISKS_ERROR_BUSY,
                             "One or more block devices are holding %s", device->priv->device_file);
                goto out;
        }

        /* If we are an extended partition, we are also busy if one or more logical partitions are busy
         * even if @check_partitions is FALSE... This is because an extended partition only really is
         * a place holder.
         */
        if (g_strcmp0 (device->priv->partition_scheme, "mbr") == 0 && device->priv->partition_type != NULL) {
                gint partition_type;
                partition_type = strtol (device->priv->partition_type, NULL, 0);
                if (partition_type == 0x05 ||
                    partition_type == 0x0f ||
                    partition_type == 0x85) {
                        DevkitDisksDevice *drive_device;
                        drive_device = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon,
                                                                                      device->priv->partition_slave);
                        if (devkit_disks_device_local_logical_partitions_are_busy (drive_device)) {
                                g_set_error (error, DEVKIT_DISKS_ERROR, DEVKIT_DISKS_ERROR_BUSY,
                                             "%s is an MS-DOS extended partition and one or more "
                                             "logical partitions are busy",
                                             device->priv->device_file);
                                goto out;
                        }
                }
        }


        /* if we are a partition table, we are busy if one of our partitions are busy */
        if (check_partitions && device->priv->device_is_partition_table) {
                if (devkit_disks_device_local_partitions_are_busy (device)) {
                        g_set_error (error, DEVKIT_DISKS_ERROR, DEVKIT_DISKS_ERROR_BUSY,
                                     "One or more partitions are busy on %s", device->priv->device_file);
                        goto out;
                }
        }

        ret = FALSE;

out:
        return ret;
}

/* note: this only checks whether the actual partitions are busy;
 * caller will need to check the main device itself too
 */
static gboolean
devkit_disks_device_local_partitions_are_busy (DevkitDisksDevice *device)
{
        gboolean ret;
        GList *l;
        GList *devices;

        ret = FALSE;

        devices = devkit_disks_daemon_local_get_all_devices (device->priv->daemon);
        for (l = devices; l != NULL; l = l->next) {
                DevkitDisksDevice *d = DEVKIT_DISKS_DEVICE (l->data);

                if (d->priv->device_is_partition &&
                    d->priv->partition_slave != NULL &&
                    g_strcmp0 (d->priv->partition_slave, device->priv->object_path) == 0) {

                        if (devkit_disks_device_local_is_busy (d, FALSE, NULL)) {
                                ret = TRUE;
                                break;
                        }
                }
        }

        g_list_free (devices);

        return ret;
}

static gboolean
devkit_disks_device_local_logical_partitions_are_busy (DevkitDisksDevice *device)
{
        gboolean ret;
        GList *l;
        GList *devices;

        ret = FALSE;

        devices = devkit_disks_daemon_local_get_all_devices (device->priv->daemon);
        for (l = devices; l != NULL; l = l->next) {
                DevkitDisksDevice *d = DEVKIT_DISKS_DEVICE (l->data);

                if (d->priv->device_is_partition &&
                    d->priv->partition_slave != NULL &&
                    g_strcmp0 (d->priv->partition_slave, device->priv->object_path) == 0 &&
                    g_strcmp0 (d->priv->partition_scheme, "mbr") == 0 &&
                    d->priv->partition_number >= 5) {

                        if (devkit_disks_device_local_is_busy (d, FALSE, NULL)) {
                                ret = TRUE;
                                break;
                        }
                }
        }

        g_list_free (devices);

        return ret;
}

void
devkit_disks_device_removed (DevkitDisksDevice *device)
{
        guint n;

        device->priv->removed = TRUE;

        /* TODO: this is in a yet to be released version of dbus-glib, use it when available

        dbus_g_connection_unregister_g_object (device->priv->system_bus_connection,
                                               G_OBJECT (device));
        */

        /* device is now removed; update all slaves and holders */
        for (n = 0; n < device->priv->slaves_objpath->len; n++) {
                const gchar *objpath2 = ((gchar **) device->priv->slaves_objpath->pdata)[n];
                DevkitDisksDevice *device2;

                device2 = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, objpath2);
                if (device2 != NULL) {
                        update_info (device2);
                }
        }
        for (n = 0; n < device->priv->holders_objpath->len; n++) {
                const gchar *objpath2 = ((gchar **) device->priv->holders_objpath->pdata)[n];
                DevkitDisksDevice *device2;

                device2 = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, objpath2);
                if (device2 != NULL) {
                        update_info (device2);
                }
        }

        /* If the device is busy, we possibly need to clean up if the
         * device itself is busy. This includes
         *
         *  - force unmounting the device and/or all it's partitions
         *
         *  - tearing down a luks mapping if it's a cleartext device
         *    backed by a crypted device
         *
         * but see force_removal() for details.
         *
         * This is the normally the path where the enclosing device is
         * removed. Compare with devkit_disks_device_changed() for the
         * other path.
         */
        force_removal (device, NULL, NULL);
}

DevkitDisksDevice *
devkit_disks_device_new (DevkitDisksDaemon *daemon, GUdevDevice *d)
{
        DevkitDisksDevice *device;
        const char *native_path;

        device = NULL;
        native_path = g_udev_device_get_sysfs_path (d);

        /* ignore ram and loop devices */
        if (g_str_has_prefix (native_path, "/sys/devices/virtual/block/ram") ||
            g_str_has_prefix (native_path, "/sys/devices/virtual/block/loop"))
                goto out;

        device = DEVKIT_DISKS_DEVICE (g_object_new (DEVKIT_DISKS_TYPE_DEVICE, NULL));
        device->priv->d = g_object_ref (d);
        device->priv->daemon = g_object_ref (daemon);
        device->priv->native_path = g_strdup (native_path);

        /* TODO: we might want to get this from udev or the kernel... to get the time when the device
         *       was initially detected... as opposed to this value which is when the device was detected
         *       by our daemon... but this will do for now...
         */
        device->priv->device_detection_time = (guint64) time (NULL);

        if (!update_info (device)) {
                g_object_unref (device);
                device = NULL;
                goto out;
        }

        if (!register_disks_device (DEVKIT_DISKS_DEVICE (device))) {
                g_object_unref (device);
                device = NULL;
                goto out;
        }

        /* if just added, update the smart data if applicable */
        if (device->priv->drive_ata_smart_is_available) {
                gchar *ata_smart_refresh_data_options[] = {NULL};
                devkit_disks_device_drive_ata_smart_refresh_data (device,
                                                                  ata_smart_refresh_data_options,
                                                                  NULL);
        }

out:
        return device;
}

static void
drain_pending_changes (DevkitDisksDevice *device, gboolean force_update)
{
        gboolean emit_changed;

        emit_changed = FALSE;

        /* the update-in-idle is set up if, and only if, there are pending changes - so
         * we should emit a 'change' event only if it is set up
         */
        if (device->priv->emit_changed_idle_id != 0) {
                g_source_remove (device->priv->emit_changed_idle_id);
                device->priv->emit_changed_idle_id = 0;
                emit_changed = TRUE;
        }

        if ((!device->priv->removed) && (emit_changed || force_update)) {
                if (device->priv->object_path != NULL) {
                        g_print ("**** EMITTING CHANGED for %s\n", device->priv->native_path);
                        g_signal_emit_by_name (device, "changed");
                        g_signal_emit_by_name (device->priv->daemon, "device-changed", device->priv->object_path);
                }
        }
}

static void
emit_job_changed (DevkitDisksDevice *device)
{
        drain_pending_changes (device, FALSE);

        if (!device->priv->removed) {
                g_print ("**** EMITTING JOB-CHANGED for %s\n", device->priv->native_path);
                g_signal_emit_by_name (device->priv->daemon,
                                       "device-job-changed",
                                       device->priv->object_path,
                                       device->priv->job_in_progress,
                                       device->priv->job_id,
                                       device->priv->job_initiated_by_uid,
                                       device->priv->job_is_cancellable,
                                       device->priv->job_percentage,
                                       NULL);
                g_signal_emit (device, signals[JOB_CHANGED_SIGNAL], 0,
                               device->priv->job_in_progress,
                               device->priv->job_id,
                               device->priv->job_initiated_by_uid,
                               device->priv->job_is_cancellable,
                               device->priv->job_percentage);
        }
}

/* called by the daemon on the 'change' uevent */
gboolean
devkit_disks_device_changed (DevkitDisksDevice *device, GUdevDevice *d, gboolean synthesized)
{
        gboolean keep_device;

        g_object_unref (device->priv->d);
        device->priv->d = g_object_ref (d);

        keep_device = update_info (device);

        /* this 'change' event might prompt us to remove the device */
        if (!keep_device)
                goto out;

        /* no, it's good .. keep it.. and always force a 'change' signal if the event isn't synthesized */
        drain_pending_changes (device, !synthesized);

        /* Check if media was removed. If so, we possibly need to clean up
         * if the device itself is busy. This includes
         *
         *  - force unmounting the device
         *
         *  - tearing down a luks mapping if it's a cleartext device
         *    backed by a crypted device
         *
         * but see force_removal() for details.
         *
         * This is the normally the path where the media is removed but the enclosing
         * device is still present. Compare with devkit_disks_device_removed() for
         * the other path.
         */
        if (!device->priv->device_is_media_available) {
                GList *l;
                GList *devices;

                force_removal (device, NULL, NULL);

                /* check all partitions */
                devices = devkit_disks_daemon_local_get_all_devices (device->priv->daemon);
                for (l = devices; l != NULL; l = l->next) {
                        DevkitDisksDevice *d = DEVKIT_DISKS_DEVICE (l->data);

                        if (d->priv->device_is_partition &&
                            d->priv->partition_slave != NULL &&
                            strcmp (d->priv->partition_slave, device->priv->object_path) == 0) {

                                force_removal (d, NULL, NULL);
                        }
                }

                g_list_free (devices);
        }
out:
        return keep_device;
}

/*--------------------------------------------------------------------------------------------------------------*/

const char *
devkit_disks_device_local_get_object_path (DevkitDisksDevice *device)
{
        return device->priv->object_path;
}

const char *
devkit_disks_device_local_get_native_path (DevkitDisksDevice *device)
{
        return device->priv->native_path;
}

dev_t
devkit_disks_device_local_get_dev (DevkitDisksDevice *device)
{
        return device->priv->dev;
}

const char *
devkit_disks_device_local_get_device_file (DevkitDisksDevice *device)
{
        return device->priv->device_file;
}

/*--------------------------------------------------------------------------------------------------------------*/

static gboolean
throw_error (DBusGMethodInvocation *context, int error_code, const char *format, ...)
{
        GError *error;
        va_list args;
        char *message;

        if (context == NULL)
                return TRUE;

        va_start (args, format);
        message = g_strdup_vprintf (format, args);
        va_end (args);

        error = g_error_new (DEVKIT_DISKS_ERROR,
                             error_code,
                             "%s", message);
        dbus_g_method_return_error (context, error);
        g_error_free (error);
        g_free (message);
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

typedef void (*JobCompletedFunc) (DBusGMethodInvocation *context,
                                  DevkitDisksDevice *device,
                                  gboolean was_cancelled,
                                  int status,
                                  const char *stderr,
                                  const char *stdout,
                                  gpointer user_data);

struct Job {
        char *job_id;

        DevkitDisksDevice *device;
        DBusGMethodInvocation *context;
        JobCompletedFunc job_completed_func;
        GPid pid;
        gpointer user_data;
        GDestroyNotify user_data_destroy_func;
        gboolean was_cancelled;

        int stderr_fd;
        GIOChannel *error_channel;
        guint error_channel_source_id;
        GString *error_string;

        int stdout_fd;
        GIOChannel *out_channel;
        guint out_channel_source_id;
        GString *stdout_string;
        int stdout_string_cursor;

        char *stdin_str;
        char *stdin_cursor;
        int stdin_fd;
        GIOChannel *in_channel;
        guint in_channel_source_id;
};

static void
job_free (Job *job)
{
        if (job->user_data_destroy_func != NULL)
                job->user_data_destroy_func (job->user_data);
        if (job->device != NULL)
                g_object_unref (job->device);
        if (job->stderr_fd >= 0)
                close (job->stderr_fd);
        if (job->stdout_fd >= 0)
                close (job->stdout_fd);
        if (job->stdin_fd >= 0) {
                close (job->stdin_fd);
                g_source_remove (job->in_channel_source_id);
                g_io_channel_unref (job->in_channel);
        }
        g_source_remove (job->error_channel_source_id);
        g_source_remove (job->out_channel_source_id);
        g_io_channel_unref (job->error_channel);
        g_io_channel_unref (job->out_channel);
        g_string_free (job->error_string, TRUE);
        /* scrub stdin (may contain secrets) */
        if (job->stdin_str != NULL) {
                memset (job->stdin_str, '\0', strlen (job->stdin_str));
        }
        g_string_free (job->stdout_string, TRUE);
        g_free (job->stdin_str);
        g_free (job->job_id);
        g_free (job);
}

static void
job_child_watch_cb (GPid pid, int status, gpointer user_data)
{
        char *buf;
        gsize buf_size;
        Job *job = user_data;

        if (g_io_channel_read_to_end (job->error_channel, &buf, &buf_size, NULL) == G_IO_STATUS_NORMAL) {
                g_string_append_len (job->error_string, buf, buf_size);
                g_free (buf);
        }
        if (g_io_channel_read_to_end (job->out_channel, &buf, &buf_size, NULL) == G_IO_STATUS_NORMAL) {
                g_string_append_len (job->stdout_string, buf, buf_size);
                g_free (buf);
        }

        g_print ("helper(pid %5d): completed with exit code %d\n", job->pid, WEXITSTATUS (status));

        if (job->device != NULL && job->job_id != NULL) {
                job->device->priv->job_in_progress = FALSE;
                g_free (job->device->priv->job_id);
                job->device->priv->job_id = NULL;
                job->device->priv->job_initiated_by_uid = 0;
                job->device->priv->job_is_cancellable = FALSE;
                job->device->priv->job_percentage = -1.0;

                job->device->priv->job = NULL;
        }

        job->job_completed_func (job->context,
                                 job->device,
                                 job->was_cancelled,
                                 status,
                                 job->error_string->str,
                                 job->stdout_string->str,
                                 job->user_data);

        if (job->device != NULL && job->job_id != NULL) {
                emit_job_changed (job->device);
        }
        job_free (job);
}

static void
job_cancel (DevkitDisksDevice *device)
{
        g_return_if_fail (device->priv->job != NULL);

        device->priv->job->was_cancelled = TRUE;

        /* TODO: maybe wait and user a bigger hammer? (SIGKILL) */
        kill (device->priv->job->pid, SIGTERM);
}

static gboolean
job_read_error (GIOChannel *channel,
                GIOCondition condition,
                gpointer user_data)
{
        char buf[1024];
        gsize bytes_read;
        Job *job = user_data;

        g_io_channel_read_chars (channel, buf, sizeof buf, &bytes_read, NULL);
        g_string_append_len (job->error_string, buf, bytes_read);
        return TRUE;
}

static gboolean
job_write_in (GIOChannel *channel,
              GIOCondition condition,
              gpointer user_data)
{
        Job *job = user_data;
        gsize bytes_written;

        if (job->stdin_cursor == NULL || job->stdin_cursor[0] == '\0') {
                /* nothing left to write; remove ourselves */
                return FALSE;
        }

        g_io_channel_write_chars (channel, job->stdin_cursor, strlen (job->stdin_cursor),
                                  &bytes_written, NULL);
        g_io_channel_flush (channel, NULL);
        job->stdin_cursor += bytes_written;
        return TRUE;
}

static gboolean
job_read_out (GIOChannel *channel,
              GIOCondition condition,
              gpointer user_data)
{
        char *s;
        char *line;
        char buf[1024];
        gsize bytes_read;
        Job *job = user_data;

        g_io_channel_read_chars (channel, buf, sizeof buf, &bytes_read, NULL);
        g_string_append_len (job->stdout_string, buf, bytes_read);

        do {
                gsize line_len;

                s = strstr (job->stdout_string->str + job->stdout_string_cursor, "\n");
                if (s == NULL)
                        break;

                line_len = s - (job->stdout_string->str + job->stdout_string_cursor);
                line = g_strndup (job->stdout_string->str + job->stdout_string_cursor, line_len);
                job->stdout_string_cursor += line_len + 1;

                //g_print ("helper(pid %5d): '%s'\n", job->pid, line);

                if (strlen (line) < 256) {
                        double cur_percentage;;

                        if (sscanf (line, "devkit-disks-helper-progress: %lg", &cur_percentage) == 1) {
                                if (job->device != NULL && job->job_id != NULL) {
                                        job->device->priv->job_percentage = cur_percentage;
                                        emit_job_changed (job->device);
                                }
                        }
                }

                g_free (line);
        } while (TRUE);

        return TRUE;
}

static void
job_local_start (DevkitDisksDevice *device,
                 const char *job_id)
{
        if (device->priv->job != NULL || device->priv->job_in_progress) {
                g_warning ("There is already a job running");
                goto out;
        }

        g_free (device->priv->job_id);
        device->priv->job_id = g_strdup (job_id);
        device->priv->job_initiated_by_uid = 0;
        device->priv->job_in_progress = TRUE;
        device->priv->job_is_cancellable = FALSE;
        device->priv->job_percentage = -1.0;

        emit_job_changed (device);
out:
        ;
}

static void
job_local_end (DevkitDisksDevice *device)
{
        if (!device->priv->job_in_progress || device->priv->job != NULL) {
                g_warning ("There is no job running");
                goto out;
        }

        device->priv->job_in_progress = FALSE;
        g_free (device->priv->job_id);
        device->priv->job_id = NULL;
        device->priv->job_initiated_by_uid = 0;
        device->priv->job_is_cancellable = FALSE;
        device->priv->job_percentage = -1.0;
        emit_job_changed (device);
out:
        ;
}

static gboolean
job_new (DBusGMethodInvocation *context,
         const char            *job_id,
         gboolean               is_cancellable,
         DevkitDisksDevice     *device,
         char                 **argv,
         const char            *stdin_str,
         JobCompletedFunc       job_completed_func,
         gpointer               user_data,
         GDestroyNotify         user_data_destroy_func)
{
        Job *job;
        gboolean ret;
        GError *error;

        ret = FALSE;
        job = NULL;

        if (device != NULL) {
                if (device->priv->job != NULL || device->priv->job_in_progress) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_BUSY,
                                     "There is already a job running");
                        goto out;
                }
        }

        job = g_new0 (Job, 1);
        job->context = context;
        job->device = device != NULL ? DEVKIT_DISKS_DEVICE (g_object_ref (device)) : NULL;
        job->job_completed_func = job_completed_func;
        job->user_data = user_data;
        job->user_data_destroy_func = user_data_destroy_func;
        job->stderr_fd = -1;
        job->stdout_fd = -1;
        job->stdin_fd = -1;
        job->stdin_str = g_strdup (stdin_str);
        job->stdin_cursor = job->stdin_str;
        job->stdout_string = g_string_sized_new (1024);
        job->job_id = g_strdup (job_id);

        if (device != NULL && job_id != NULL) {
                g_free (job->device->priv->job_id);
                job->device->priv->job_id = g_strdup (job_id);
        }

        error = NULL;
        if (!g_spawn_async_with_pipes (NULL,
                                       argv,
                                       NULL,
                                       G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                       NULL,
                                       NULL,
                                       &(job->pid),
                                       stdin_str != NULL ? &(job->stdin_fd) : NULL,
                                       &(job->stdout_fd),
                                       &(job->stderr_fd),
                                       &error)) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED, "Error starting job: %s", error->message);
                g_error_free (error);
                goto out;
        }

        g_child_watch_add (job->pid, job_child_watch_cb, job);

        job->error_string = g_string_new ("");
        job->error_channel = g_io_channel_unix_new (job->stderr_fd);
        error = NULL;
        if (g_io_channel_set_flags (job->error_channel, G_IO_FLAG_NONBLOCK, &error) != G_IO_STATUS_NORMAL) {
                g_warning ("Cannon set stderr fd for child to be non blocking: %s", error->message);
                g_error_free (error);
        }
        job->error_channel_source_id = g_io_add_watch (job->error_channel, G_IO_IN, job_read_error, job);

        job->out_channel = g_io_channel_unix_new (job->stdout_fd);
        error = NULL;
        if (g_io_channel_set_flags (job->out_channel, G_IO_FLAG_NONBLOCK, &error) != G_IO_STATUS_NORMAL) {
                g_warning ("Cannon set stdout fd for child to be non blocking: %s", error->message);
                g_error_free (error);
        }
        job->out_channel_source_id = g_io_add_watch (job->out_channel, G_IO_IN, job_read_out, job);

        if (job->stdin_fd >= 0) {
                job->in_channel = g_io_channel_unix_new (job->stdin_fd);
                if (g_io_channel_set_flags (job->in_channel, G_IO_FLAG_NONBLOCK, &error) != G_IO_STATUS_NORMAL) {
                        g_warning ("Cannon set stdin fd for child to be non blocking: %s", error->message);
                        g_error_free (error);
                }
                job->in_channel_source_id = g_io_add_watch (job->in_channel, G_IO_OUT, job_write_in, job);
        }

        ret = TRUE;

        if (device != NULL && job_id != NULL) {
                device->priv->job_in_progress = TRUE;
                device->priv->job_is_cancellable = is_cancellable;
                device->priv->job_percentage = -1.0;
                device->priv->job_initiated_by_uid = 0;
                if (context != NULL) {
                        devkit_disks_daemon_local_get_uid (device->priv->daemon,
                                                           &(device->priv->job_initiated_by_uid),
                                                           context);
                }

                device->priv->job = job;

                emit_job_changed (device);
        }

        if (device != NULL) {
                g_print ("helper(pid %5d): launched job %s on %s\n", job->pid, argv[0], device->priv->device_file);
        } else {
                g_print ("helper(pid %5d): launched job %s on daemon\n", job->pid, argv[0]);
        }

out:
        if (!ret && job != NULL)
                job_free (job);
        return ret;
}

/*--------------------------------------------------------------------------------------------------------------*/
/* exported methods */

typedef struct {
        char *mount_point;
        gboolean remove_dir_on_unmount;
} MountData;

static MountData *
filesystem_mount_data_new (const char *mount_point, gboolean remove_dir_on_unmount)
{
        MountData *data;
        data = g_new0 (MountData, 1);
        data->mount_point = g_strdup (mount_point);
        data->remove_dir_on_unmount = remove_dir_on_unmount;
        return data;
}

static void
filesystem_mount_data_free (MountData *data)
{
        g_free (data->mount_point);
        g_free (data);
}

static gboolean
is_device_in_fstab (DevkitDisksDevice *device, char **out_mount_point)
{
        GList *l;
        GList *mount_points;
        gboolean ret;

        ret = FALSE;

        mount_points = g_unix_mount_points_get (NULL);
        for (l = mount_points; l != NULL; l = l->next) {
                GUnixMountPoint *mount_point = l->data;
                char canonical_device_file[PATH_MAX];
                char *device_path;
                char *s;

                device_path = g_strdup (g_unix_mount_point_get_device_path (mount_point));

                /* get the canonical path; e.g. resolve
                 *
                 * /dev/disk/by-path/pci-0000:00:1d.7-usb-0:3:1.0-scsi-0:0:0:3-part5
                 * UUID=78af6939-adac-4ea5-a2a8-576e141da010
                 * LABEL=foobar
                 *
                 * into something like /dev/sde5.
                 */
                if (g_str_has_prefix (device_path, "UUID=")) {
                        s = device_path;
                        device_path = g_strdup_printf ("/dev/disk/by-uuid/%s", device_path + 5);
                        g_free (s);
                } else if (g_str_has_prefix (device_path, "LABEL=")) {
                        s = device_path;
                        device_path = g_strdup_printf ("/dev/disk/by-label/%s", device_path + 6);
                        g_free (s);
                }

                if (realpath (device_path, canonical_device_file) == NULL) {
                        g_free (device_path);
                        continue;
                }
                g_free (device_path);

                if (strcmp (device->priv->device_file, canonical_device_file) == 0) {
                        ret = TRUE;
                        if (out_mount_point != NULL)
                                *out_mount_point = g_strdup (g_unix_mount_point_get_mount_path (mount_point));
                        break;
                }
        }
        g_list_foreach (mount_points, (GFunc) g_unix_mount_point_free, NULL);
        g_list_free (mount_points);

        return ret;
}

typedef struct {
        const char         *fstype;
        const char * const *defaults;
        const char * const *allow;
        const char * const *allow_uid_self;
        const char * const *allow_gid_self;
} FSMountOptions;

/* ---------------------- vfat -------------------- */

static const char *vfat_defaults[] =       {"uid=",
                                            "gid=",
                                            "shortname=lower",
                                            "dmask=0077",
                                            "utf8=1",
                                            NULL};
static const char *vfat_allow[] =          {"flush",
                                            "utf8=",
                                            "shortname=",
                                            "umask=",
                                            "dmask=",
                                            "fmask=",
                                            "codepage=",
                                            "iocharset=",
                                            NULL};
static const char *vfat_allow_uid_self[] = {"uid=", NULL};
static const char *vfat_allow_gid_self[] = {"gid=", NULL};

/* ---------------------- ntfs -------------------- */
/* this is assuming that ntfs-3g is used */

static const char *ntfs_defaults[] =       {"uid=",
                                            "gid=",
                                            "dmask=0077",
                                            NULL};
static const char *ntfs_allow[] =          {"umask=",
                                            "dmask=",
                                            "fmask=",
                                            NULL};
static const char *ntfs_allow_uid_self[] = {"uid=", NULL};
static const char *ntfs_allow_gid_self[] = {"gid=", NULL};

/* ---------------------- iso9660 -------------------- */

static const char *iso9660_defaults[] =       {"uid=",
                                               "gid=",
                                               "iocharset=utf8",
                                               "mode=0400",
                                               "dmode=0500",
                                               NULL};
static const char *iso9660_allow[] =          {"norock",
                                               "nojoliet",
                                               "iocharset=",
                                               "mode=",
                                               "dmode=",
                                               NULL};
static const char *iso9660_allow_uid_self[] = {"uid=", NULL};
static const char *iso9660_allow_gid_self[] = {"gid=", NULL};

/* ---------------------- udf -------------------- */

static const char *udf_defaults[] =       {"uid=",
                                           "gid=",
                                           "iocharset=utf8",
                                           "umask=0077",
                                           NULL};
static const char *udf_allow[] =          {"iocharset=",
                                           "umask=",
                                           NULL};
static const char *udf_allow_uid_self[] = {"uid=", NULL};
static const char *udf_allow_gid_self[] = {"gid=", NULL};

/* ------------------------------------------------ */
/* TODO: support context= */

static const char *any_allow[] = {"exec",
                                  "noexec",
                                  "nodev",
                                  "nosuid",
                                  "atime",
                                  "noatime",
                                  "nodiratime",
                                  "ro",
                                  "rw",
                                  "sync",
                                  "dirsync",
                                  NULL};

static const FSMountOptions fs_mount_options[] = {
        {"vfat", vfat_defaults, vfat_allow, vfat_allow_uid_self, vfat_allow_gid_self},
        {"ntfs", ntfs_defaults, ntfs_allow, ntfs_allow_uid_self, ntfs_allow_gid_self},
        {"iso9660", iso9660_defaults, iso9660_allow, iso9660_allow_uid_self, iso9660_allow_gid_self},
        {"udf", udf_defaults, udf_allow, udf_allow_uid_self, udf_allow_gid_self},
};

/* ------------------------------------------------ */

static int num_fs_mount_options = sizeof (fs_mount_options) / sizeof (FSMountOptions);

static const FSMountOptions *
find_mount_options_for_fs (const char *fstype)
{
        int n;
        const FSMountOptions *fsmo;

        for (n = 0; n < num_fs_mount_options; n++) {
                fsmo = fs_mount_options + n;
                if (strcmp (fsmo->fstype, fstype) == 0)
                        goto out;
        }

        fsmo = NULL;
out:
        return fsmo;
}

static gid_t
find_primary_gid (uid_t uid)
{
        struct passwd *pw;
        gid_t gid;

        gid = (gid_t) -1;

        pw = getpwuid (uid);
        if (pw == NULL) {
                g_warning ("Couldn't look up uid %d: %m", uid);
                goto out;
        }
        gid = pw->pw_gid;

out:
        return gid;
}

static gboolean
is_uid_in_gid (uid_t uid, gid_t gid)
{
        gboolean ret;
        struct passwd *pw;
        static gid_t supplementary_groups[128];
        int num_supplementary_groups = 128;
        int n;

        /* TODO: use some #define instead of harcoding some random number like 128 */

        ret = FALSE;

        pw = getpwuid (uid);
        if (pw == NULL) {
                g_warning ("Couldn't look up uid %d: %m", uid);
                goto out;
        }
        if (pw->pw_gid == gid) {
                ret = TRUE;
                goto out;
        }

        if (getgrouplist (pw->pw_name, pw->pw_gid, supplementary_groups, &num_supplementary_groups) < 0) {
                g_warning ("Couldn't find supplementary groups for uid %d: %m", uid);
                goto out;
        }

        for (n = 0; n < num_supplementary_groups; n++) {
                if (supplementary_groups[n] == gid) {
                        ret = TRUE;
                        goto out;
                }
        }

out:
        return ret;
}

static gboolean
is_mount_option_allowed (const FSMountOptions *fsmo,
                         const char *option,
                         uid_t caller_uid)
{
        int n;
        char *endp;
        uid_t uid;
        gid_t gid;
        gboolean allowed;
        const char *ep;
        gsize ep_len;

        allowed = FALSE;

        /* first run through the allowed mount options */
        if (fsmo != NULL) {
                for (n = 0; fsmo->allow != NULL && fsmo->allow[n] != NULL; n++) {
                        ep = strstr (fsmo->allow[n], "=");
                        if (ep != NULL && ep[1] == '\0') {
                                ep_len = ep - fsmo->allow[n] + 1;
                                if (strncmp (fsmo->allow[n], option, ep_len) == 0) {
                                        allowed = TRUE;
                                        goto out;
                                }
                        } else {
                                if (strcmp (fsmo->allow[n], option) == 0) {
                                        allowed = TRUE;
                                        goto out;
                                }
                        }
                }
        }
        for (n = 0; any_allow[n] != NULL; n++) {
                ep = strstr (any_allow[n], "=");
                if (ep != NULL && ep[1] == '\0') {
                        ep_len = ep - any_allow[n] + 1;
                        if (strncmp (any_allow[n], option, ep_len) == 0) {
                                allowed = TRUE;
                                goto out;
                        }
                } else {
                        if (strcmp (any_allow[n], option) == 0) {
                                allowed = TRUE;
                                goto out;
                        }
                }
        }

        /* .. then check for mount options where the caller is allowed to pass
         * in his own uid
         */
        if (fsmo != NULL) {
                for (n = 0; fsmo->allow_uid_self != NULL && fsmo->allow_uid_self[n] != NULL; n++) {
                        const char *r_mount_option = fsmo->allow_uid_self[n];
                        if (g_str_has_prefix (option, r_mount_option)) {
                                uid = strtol (option + strlen (r_mount_option), &endp, 10);
                                if (*endp != '\0')
                                        continue;
                                if (uid == caller_uid) {
                                        allowed = TRUE;
                                        goto out;
                                }
                        }
                }
        }

        /* .. ditto for gid
         */
        if (fsmo != NULL) {
                for (n = 0; fsmo->allow_gid_self != NULL && fsmo->allow_gid_self[n] != NULL; n++) {
                        const char *r_mount_option = fsmo->allow_gid_self[n];
                        if (g_str_has_prefix (option, r_mount_option)) {
                                gid = strtol (option + strlen (r_mount_option), &endp, 10);
                                if (*endp != '\0')
                                        continue;
                                if (is_uid_in_gid (caller_uid, gid)) {
                                        allowed = TRUE;
                                        goto out;
                                }
                        }
                }
        }

out:
        return allowed;
}

static char **
prepend_default_mount_options (const FSMountOptions *fsmo, uid_t caller_uid, char **given_options)
{
        GPtrArray *options;
        int n;
        char *s;
        gid_t gid;

        options = g_ptr_array_new ();
        if (fsmo != NULL) {
                for (n = 0; fsmo->defaults != NULL && fsmo->defaults[n] != NULL; n++) {
                        const char *option = fsmo->defaults[n];

                        if (strcmp (option, "uid=") == 0) {
                                s = g_strdup_printf ("uid=%d", caller_uid);
                                g_ptr_array_add (options, s);
                        } else if (strcmp (option, "gid=") == 0) {
                                gid = find_primary_gid (caller_uid);
                                if (gid != (gid_t) -1) {
                                        s = g_strdup_printf ("gid=%d", gid);
                                        g_ptr_array_add (options, s);
                                }
                        } else {
                                g_ptr_array_add (options, g_strdup (option));
                        }
                }
        }
        for (n = 0; given_options[n] != NULL; n++) {
                g_ptr_array_add (options, g_strdup (given_options[n]));
        }

        g_ptr_array_add (options, NULL);

        return (char **) g_ptr_array_free (options, FALSE);
}

static void
filesystem_mount_completed_cb (DBusGMethodInvocation *context,
                               DevkitDisksDevice *device,
                               gboolean job_was_cancelled,
                               int status,
                               const char *stderr,
                               const char *stdout,
                               gpointer user_data)
{
        MountData *data = (MountData *) user_data;
        uid_t uid;

        devkit_disks_daemon_local_get_uid (device->priv->daemon, &uid, context);

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {

                update_info (device);
                drain_pending_changes (device, FALSE);

                dbus_g_method_return (context, data->mount_point);
        } else {
                if (data->remove_dir_on_unmount) {
                        devkit_disks_mount_file_remove (device->priv->device_file, data->mount_point);
                        if (g_rmdir (data->mount_point) != 0) {
                                g_warning ("Error removing dir in late mount error path: %m");
                        }
                }

                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error mounting: mount exited with exit code %d: %s",
                                     WEXITSTATUS (status), stderr);
                }
        }
}

static void
devkit_disks_device_filesystem_mount_authorized_cb (DevkitDisksDaemon     *daemon,
                                                    DevkitDisksDevice     *device,
                                                    DBusGMethodInvocation *context,
                                                    const gchar           *action_id,
                                                    guint                  num_user_data,
                                                    gpointer              *user_data_elements)
{
        const gchar *filesystem_type = user_data_elements[0];
        gchar **given_options = user_data_elements[1];
        int n;
        GString *s;
        char *argv[10];
        char *mount_point;
        char *fstype;
        char *mount_options;
        GError *error;
        uid_t caller_uid;
        gboolean remove_dir_on_unmount;
        const FSMountOptions *fsmo;
        char **options;
        char uid_buf[32];

        fstype = NULL;
        options = NULL;
        mount_options = NULL;
        mount_point = NULL;
        remove_dir_on_unmount = FALSE;
        error = NULL;

        devkit_disks_daemon_local_get_uid (device->priv->daemon, &caller_uid, context);

        if (device->priv->id_usage == NULL ||
            strcmp (device->priv->id_usage, "filesystem") != 0) {
                if ((g_strcmp0 (filesystem_type, "auto") == 0 || g_strcmp0 (filesystem_type, "") == 0) &&
                    device->priv->id_usage == NULL) {
                        /* if we don't know the usage of the device and 'auto' or '' is passed for fstype
                         * then just try that.. this is to make, for example, mounting /dev/fd0 work (we
                         * don't probe such devices for filesystems in udev)
                         */
                } else {
                        throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                                     "Not a mountable file system");
                        goto out;
                }
        }

        if (devkit_disks_device_local_is_busy (device, FALSE, &error)) {
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        /* Check if the device is referenced in /etc/fstab; if so, attempt to
         * mount the device as the user
         */
        if (is_device_in_fstab (device, &mount_point)) {
                n = 0;
                snprintf (uid_buf, sizeof uid_buf, "%d", caller_uid);
                argv[n++] = PACKAGE_LIBEXEC_DIR "/devkit-disks-helper-fstab-mounter";
                argv[n++] = "mount";
                argv[n++] = device->priv->device_file;
                argv[n++] = uid_buf;
                argv[n++] = NULL;
                goto run_job;
        }

        /* set the fstype */
        fstype = NULL;
        if (strlen (filesystem_type) == 0) {
                if (device->priv->id_type != NULL && strlen (device->priv->id_type) > 0) {
                        fstype = g_strdup (device->priv->id_type);
                } else {
                        fstype = g_strdup ("auto");
                }
        } else {
                fstype = g_strdup (filesystem_type);
        }

        fsmo = find_mount_options_for_fs (fstype);

        /* always prepend some reasonable default mount options; these are
         * chosen here; the user can override them if he wants to
         */
        options = prepend_default_mount_options (fsmo, caller_uid, given_options);

        /* validate mount options and check for authorizations */
        s = g_string_new ("uhelper=devkit,nodev,nosuid");
        for (n = 0; options[n] != NULL; n++) {
                const char *option = options[n];

                /* avoid attacks like passing "shortname=lower,uid=0" as a single mount option */
                if (strstr (option, ",") != NULL) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_INVALID_OPTION,
                                     "Malformed mount option: ", option);
                        g_string_free (s, TRUE);
                        goto out;
                }

                /* first check if the mount option is allowed */
                if (!is_mount_option_allowed (fsmo, option, caller_uid)) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_INVALID_OPTION,
                                     "Mount option %s is not allowed", option);
                        g_string_free (s, TRUE);
                        goto out;
                }

                g_string_append_c (s, ',');
                g_string_append (s, option);
        }
        mount_options = g_string_free (s, FALSE);

        g_print ("**** USING MOUNT OPTIONS '%s' FOR DEVICE %s\n", mount_options, device->priv->device_file);

        if (device->priv->device_is_mounted) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is already mounted");
                goto out;
        }

        /* Determine the mount point to use.
         *
         * TODO: use characteristics of the drive such as the name, connection etc.
         *       to get better names (/media/disk is kinda lame).
         */
        if (device->priv->id_label != NULL && strlen (device->priv->id_label) > 0 ) {
                GString *s;

                s = g_string_new ("/media/");
                for (n = 0; device->priv->id_label[n] != '\0'; n++) {
                        gint c = device->priv->id_label[n];
                        if (c == '/')
                                g_string_append_c (s, '_');
                        else
                                g_string_append_c (s, c);
                }

                mount_point = g_string_free (s, FALSE);
        } else if (device->priv->id_uuid != NULL && strlen (device->priv->id_uuid) > 0) {

                GString *s;

                s = g_string_new ("/media/");
                for (n = 0; device->priv->id_uuid[n] != '\0'; n++) {
                        gint c = device->priv->id_uuid[n];
                        if (c == '/')
                                g_string_append_c (s, '_');
                        else
                                g_string_append_c (s, c);
                }

                mount_point = g_string_free (s, FALSE);

        } else {
                mount_point = g_strdup ("/media/disk");
        }

try_another_mount_point:
        /* ... then uniqify the mount point and mkdir it */
        if (g_file_test (mount_point, G_FILE_TEST_EXISTS)) {
                char *s = mount_point;
                /* TODO: append numbers instead of _, __ and so on */
                mount_point = g_strdup_printf ("%s_", mount_point);
                g_free (s);
                goto try_another_mount_point;
        }

        remove_dir_on_unmount = TRUE;

        if (g_mkdir (mount_point, 0700) != 0) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED, "Error creating moint point: %m");
                goto out;
        }

        /* now that we have a mount point, immediately add it to the
         * /var/lib/DeviceKit-disks/mtab file.
         *
         * If mounting fails we'll clean it up in filesystem_mount_completed_cb. If it
         * hangs we'll clean it up the next time we start up.
         */
        devkit_disks_mount_file_add (device->priv->device_file,
                                     mount_point,
                                     caller_uid,
                                     remove_dir_on_unmount);

        n = 0;
        argv[n++] = "mount";
        argv[n++] = "-t";
        argv[n++] = fstype;
        argv[n++] = "-o";
        argv[n++] = mount_options;
        argv[n++] = device->priv->device_file;
        argv[n++] = mount_point;
        argv[n++] = NULL;

run_job:

        error = NULL;
        if (!job_new (context,
                      "FilesystemMount",
                      FALSE,
                      device,
                      argv,
                      NULL,
                      filesystem_mount_completed_cb,
                      filesystem_mount_data_new (mount_point, remove_dir_on_unmount),
                      (GDestroyNotify) filesystem_mount_data_free)) {
                if (remove_dir_on_unmount) {
                        devkit_disks_mount_file_remove (device->priv->device_file, mount_point);
                        if (g_rmdir (mount_point) != 0) {
                                g_warning ("Error removing dir in early mount error path: %m");
                        }
                }
                goto out;
        }

out:
        g_free (fstype);
        g_free (mount_options);
        g_free (mount_point);
        g_strfreev (options);
}

gboolean
devkit_disks_device_filesystem_mount (DevkitDisksDevice     *device,
                                      const char            *filesystem_type,
                                      char                 **given_options,
                                      DBusGMethodInvocation *context)
{
        const gchar *action_id;
        gboolean auth_no_user_interaction;
        gchar **options_to_pass;
        guint n;
        guint m;

        if (is_device_in_fstab (device, NULL)) {
                action_id = NULL;
        } else {
                if (device->priv->device_is_system_internal)
                        action_id = "org.freedesktop.devicekit.disks.filesystem-mount-system-internal";
                else
                        action_id = "org.freedesktop.devicekit.disks.filesystem-mount";
        }

        auth_no_user_interaction = FALSE;
        options_to_pass = g_strdupv (given_options);
        for (n = 0; options_to_pass != NULL && options_to_pass[n] != NULL; n++) {
                if (g_strcmp0 (options_to_pass[n], "auth_no_user_interaction") == 0) {
                        auth_no_user_interaction = TRUE;
                        g_free (options_to_pass[n]);
                        for (m = n; options_to_pass[m + 1] != NULL; m++)
                                options_to_pass[m] = options_to_pass[m + 1];
                        options_to_pass[m] = NULL;
                        break;
                }
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              action_id,
                                              "FilesystemMount",
                                              !auth_no_user_interaction,
                                              devkit_disks_device_filesystem_mount_authorized_cb,
                                              context,
                                              2,
                                              g_strdup (filesystem_type), g_free,
                                              options_to_pass, g_strfreev);

         return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
filesystem_unmount_completed_cb (DBusGMethodInvocation *context,
                                 DevkitDisksDevice *device,
                                 gboolean job_was_cancelled,
                                 int status,
                                 const char *stderr,
                                 const char *stdout,
                                 gpointer user_data)
{
        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {
                /* update_info_mount_state() will update the mounts file and clean up the directory if needed */
                update_info (device);
                dbus_g_method_return (context);
        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        if (strstr (stderr, "device is busy") != NULL) {
                                throw_error (context,
                                             DEVKIT_DISKS_ERROR_BUSY,
                                             "Cannot unmount because file system on device is busy");
                        } else {
                                throw_error (context,
                                             DEVKIT_DISKS_ERROR_FAILED,
                                             "Error unmounting: umount exited with exit code %d: %s",
                                             WEXITSTATUS (status),
                                             stderr);
                        }
                }
        }
}

static void
devkit_disks_device_filesystem_unmount_authorized_cb (DevkitDisksDaemon     *daemon,
                                                      DevkitDisksDevice     *device,
                                                      DBusGMethodInvocation *context,
                                                      const gchar           *action_id,
                                                      guint                  num_user_data,
                                                      gpointer              *user_data_elements)
{
        gchar **options = user_data_elements[0];
        int n;
        char *argv[16];
        GError *error;
        gboolean force_unmount;
        char *mount_path;
        uid_t uid;
        gchar uid_buf[32];

        mount_path = NULL;

        if (!device->priv->device_is_mounted ||
            device->priv->device_mount_paths->len == 0) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not mounted");
                goto out;
        }

        force_unmount = FALSE;
        for (n = 0; options[n] != NULL; n++) {
                char *option = options[n];
                if (strcmp ("force", option) == 0) {
                        force_unmount = TRUE;
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_INVALID_OPTION,
                                     "Unknown option %s", option);
                        goto out;
                }
        }

        devkit_disks_daemon_local_get_uid (device->priv->daemon, &uid, context);
        g_snprintf (uid_buf, sizeof uid_buf, "%d", uid);

        if (!devkit_disks_mount_file_has_device (device->priv->device_file, NULL, NULL)) {
                if (is_device_in_fstab (device, &mount_path)) {

                        n = 0;
                        argv[n++] = PACKAGE_LIBEXEC_DIR "/devkit-disks-helper-fstab-mounter";
                        if (force_unmount)
                                argv[n++] = "force_unmount";
                        else
                                argv[n++] = "unmount";
                        argv[n++] = device->priv->device_file;
                        argv[n++] = uid_buf;
                        argv[n++] = NULL;
                        goto run_job;
                }

                /* otherwise the user will have the .unmount-others authorization per the logic in
                 * devkit_disks_device_filesystem_unmount()
                 */
        }

        mount_path = g_strdup (((gchar **) device->priv->device_mount_paths->pdata)[0]);

        n = 0;
        argv[n++] = "umount";
        if (force_unmount) {
                /* on Linux we currently only have lazy unmount to emulate this */
                argv[n++] = "-l";
        }
        argv[n++] = mount_path;
        argv[n++] = NULL;

run_job:
        error = NULL;
        if (!job_new (context,
                      "FilesystemUnmount",
                      FALSE,
                      device,
                      argv,
                      NULL,
                      filesystem_unmount_completed_cb,
                      g_strdup (mount_path),
                      g_free)) {
                goto out;
        }

out:
        g_free (mount_path);
}

gboolean
devkit_disks_device_filesystem_unmount (DevkitDisksDevice     *device,
                                        char                 **options,
                                        DBusGMethodInvocation *context)
{
        const gchar *action_id;
        uid_t uid_of_mount;

        if (!device->priv->device_is_mounted ||
            device->priv->device_mount_paths->len == 0) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not mounted");
                goto out;
        }

        /* if device is in /etc/fstab, then we'll run unmount as the calling user */
        action_id = NULL;
        if (!devkit_disks_mount_file_has_device (device->priv->device_file, &uid_of_mount, NULL)) {
                if (!is_device_in_fstab (device, NULL)) {
                        action_id = "org.freedesktop.devicekit.disks.filesystem-unmount-others";
                }
        } else {
                uid_t uid;
                devkit_disks_daemon_local_get_uid (device->priv->daemon, &uid, context);
                if (uid_of_mount != uid) {
                        action_id = "org.freedesktop.devicekit.disks.filesystem-unmount-others";
                }
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              action_id,
                                              "FilesystemUnmount",
                                              TRUE,
                                              devkit_disks_device_filesystem_unmount_authorized_cb,
                                              context,
                                              1,
                                              g_strdupv (options), g_strfreev);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static uid_t
get_uid_for_pid (pid_t pid)
{
        uid_t ret;
        char proc_name[32];
        struct stat statbuf;

        ret = 0;

        snprintf (proc_name, sizeof (proc_name), "/proc/%d/stat", pid);
        if (stat (proc_name, &statbuf) == 0) {
                ret = statbuf.st_uid;
        }

        return ret;
}

static char *
get_command_line_for_pid (pid_t pid)
{
        char proc_name[32];
        char *buf;
        gsize len;
        char *ret;
        unsigned int n;

        ret = NULL;

        snprintf (proc_name, sizeof (proc_name), "/proc/%d/cmdline", pid);
        if (g_file_get_contents (proc_name, &buf, &len, NULL)) {
                for (n = 0; n < len; n++) {
                        if (buf[n] == '\0')
                                buf[n] = ' ';
                }
                g_strstrip (buf);
                ret = buf;
        }

        return ret;
}

static void
lsof_parse (const char *stdout, GPtrArray *processes)
{
        int n;
        char **tokens;

        tokens = g_strsplit (stdout, "\n", 0);
        for (n = 0; tokens[n] != NULL; n++) {
                pid_t pid;
                uid_t uid;
                char *command_line;
                GValue elem = {0};

                if (strlen (tokens[n]) == 0)
                        continue;

                pid = strtol (tokens[n], NULL, 0);
                uid = get_uid_for_pid (pid);
                command_line = get_command_line_for_pid (pid);

                g_value_init (&elem, LSOF_DATA_STRUCT_TYPE);
                g_value_take_boxed (&elem, dbus_g_type_specialized_construct (LSOF_DATA_STRUCT_TYPE));
                dbus_g_type_struct_set (&elem,
                                        0, pid,
                                        1, uid,
                                        2, command_line != NULL ? command_line : "",
                                        G_MAXUINT);
                g_ptr_array_add (processes, g_value_get_boxed (&elem));

                g_free (command_line);
        }
        g_strfreev (tokens);
}

static void
filesystem_list_open_files_completed_cb (DBusGMethodInvocation *context,
                                         DevkitDisksDevice *device,
                                         gboolean job_was_cancelled,
                                         int status,
                                         const char *stderr,
                                         const char *stdout,
                                         gpointer user_data)
{
        if ((WEXITSTATUS (status) == 0 || WEXITSTATUS (status) == 1) && !job_was_cancelled) {
                GPtrArray *processes;

                processes = g_ptr_array_new ();
                lsof_parse (stdout, processes);
                dbus_g_method_return (context, processes);
                g_ptr_array_foreach (processes, (GFunc) g_value_array_free, NULL);
                g_ptr_array_free (processes, TRUE);
        } else {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Error listing open files: lsof exited with exit code %d: %s",
                             WEXITSTATUS (status),
                             stderr);
        }
}

static void
devkit_disks_device_filesystem_list_open_files_authorized_cb (DevkitDisksDaemon     *daemon,
                                                              DevkitDisksDevice     *device,
                                                              DBusGMethodInvocation *context,
                                                              const gchar           *action_id,
                                                              guint                  num_user_data,
                                                              gpointer              *user_data_elements)
{
        int n;
        char *argv[16];
        GError *error;

        if (!device->priv->device_is_mounted ||
            device->priv->device_mount_paths->len == 0) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not mounted");
                goto out;
        }

        n = 0;
        argv[n++] = "lsof";
        argv[n++] = "-t";
        argv[n++] = ((gchar **) device->priv->device_mount_paths->pdata)[0];
        argv[n++] = NULL;

        error = NULL;
        if (!job_new (context,
                      NULL,     /* don't run this as a job */
                      FALSE,
                      device,
                      argv,
                      NULL,
                      filesystem_list_open_files_completed_cb,
                      NULL,
                      NULL)) {
                goto out;
        }

out:
        ;
}

gboolean
devkit_disks_device_filesystem_list_open_files (DevkitDisksDevice     *device,
                                                DBusGMethodInvocation *context)
{
        if (!device->priv->device_is_mounted ||
            device->priv->device_mount_paths->len == 0) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not mounted");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              device->priv->device_is_system_internal ?
                                               "org.freedesktop.devicekit.disks.filesystem-lsof-system-internal" :
                                               "org.freedesktop.devicekit.disks.filesystem-lsof",
                                              "FilesystemListOpenFiles",
                                              TRUE,
                                              devkit_disks_device_filesystem_list_open_files_authorized_cb,
                                              context,
                                              0);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
drive_eject_completed_cb (DBusGMethodInvocation *context,
                          DevkitDisksDevice *device,
                          gboolean job_was_cancelled,
                          int status,
                          const char *stderr,
                          const char *stdout,
                          gpointer user_data)
{
        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {
                /* TODO: probably wait for has_media to change to FALSE */
                dbus_g_method_return (context);
        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error ejecting: eject exited with exit code %d: %s",
                                     WEXITSTATUS (status),
                                     stderr);
                }
        }
}

static void
devkit_disks_device_drive_eject_authorized_cb (DevkitDisksDaemon     *daemon,
                                               DevkitDisksDevice     *device,
                                               DBusGMethodInvocation *context,
                                               const gchar           *action_id,
                                               guint                  num_user_data,
                                               gpointer              *user_data_elements)
{
        gchar **options = user_data_elements[0];
        int n;
        char *argv[16];
        GError *error;
        char *mount_path;

        error = NULL;
        mount_path = NULL;

        if (!device->priv->device_is_drive) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a drive");
                goto out;
        }

        if (!device->priv->device_is_media_available) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "No media in drive");
                goto out;
        }

        if (devkit_disks_device_local_is_busy (device, TRUE, &error)) {
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        for (n = 0; options[n] != NULL; n++) {
                const char *option = options[n];
                throw_error (context,
                             DEVKIT_DISKS_ERROR_INVALID_OPTION,
                             "Unknown option %s", option);
                goto out;
        }

        n = 0;
        argv[n++] = "eject";
        argv[n++] = device->priv->device_file;
        argv[n++] = NULL;

        if (!job_new (context,
                      "DriveEject",
                      FALSE,
                      device,
                      argv,
                      NULL,
                      drive_eject_completed_cb,
                      NULL,
                      NULL)) {
                goto out;
        }

out:
        ;
}

gboolean
devkit_disks_device_drive_eject (DevkitDisksDevice     *device,
                                 char                 **options,
                                 DBusGMethodInvocation *context)
{
        if (!device->priv->device_is_drive) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a drive");
                goto out;
        }

        if (!device->priv->device_is_media_available) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "No media in drive");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              "org.freedesktop.devicekit.disks.drive-eject",
                                              "DriveEject",
                                              TRUE,
                                              devkit_disks_device_drive_eject_authorized_cb,
                                              context,
                                              1,
                                              g_strdupv (options), g_strfreev);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
drive_detach_completed_cb (DBusGMethodInvocation *context,
                          DevkitDisksDevice *device,
                          gboolean job_was_cancelled,
                          int status,
                          const char *stderr,
                          const char *stdout,
                          gpointer user_data)
{
        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {
                /* TODO: probably wait for has_media to change to FALSE */
                dbus_g_method_return (context);
        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error detaching: helper exited with exit code %d: %s",
                                     WEXITSTATUS (status),
                                     stderr);
                }
        }
}

static void
devkit_disks_device_drive_detach_authorized_cb (DevkitDisksDaemon     *daemon,
                                               DevkitDisksDevice     *device,
                                               DBusGMethodInvocation *context,
                                               const gchar           *action_id,
                                               guint                  num_user_data,
                                               gpointer              *user_data_elements)
{
        gchar **options = user_data_elements[0];
        int n;
        char *argv[16];
        GError *error;
        char *mount_path;

        error = NULL;
        mount_path = NULL;

        if (!device->priv->device_is_drive) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a drive");
                goto out;
        }

        if (!device->priv->drive_can_detach) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not detachable");
                goto out;
        }

        if (devkit_disks_device_local_is_busy (device, TRUE, &error)) {
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        for (n = 0; options[n] != NULL; n++) {
                const char *option = options[n];
                throw_error (context,
                             DEVKIT_DISKS_ERROR_INVALID_OPTION,
                             "Unknown option %s", option);
                goto out;
        }

        n = 0;
        argv[n++] = PACKAGE_LIBEXEC_DIR "/devkit-disks-helper-drive-detach";
        argv[n++] = device->priv->device_file;
        argv[n++] = device->priv->native_path;
        argv[n++] = NULL;

        if (!job_new (context,
                      "DriveDetach",
                      FALSE,
                      device,
                      argv,
                      NULL,
                      drive_detach_completed_cb,
                      NULL,
                      NULL)) {
                goto out;
        }

out:
        ;
}

gboolean
devkit_disks_device_drive_detach (DevkitDisksDevice     *device,
                                 char                 **options,
                                 DBusGMethodInvocation *context)
{
        if (!device->priv->device_is_drive) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a drive");
                goto out;
        }

        if (!device->priv->drive_can_detach) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not detachable");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              "org.freedesktop.devicekit.disks.drive-detach",
                                              "DriveDetach",
                                              TRUE,
                                              devkit_disks_device_drive_detach_authorized_cb,
                                              context,
                                              1,
                                              g_strdupv (options), g_strfreev);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
filesystem_check_completed_cb (DBusGMethodInvocation *context,
                               DevkitDisksDevice *device,
                               gboolean job_was_cancelled,
                               int status,
                               const char *stderr,
                               const char *stdout,
                               gpointer user_data)
{
        if (WIFEXITED (status) && !job_was_cancelled) {
                int rc;
                gboolean fs_is_clean;

                fs_is_clean = FALSE;

                rc = WEXITSTATUS (status);
                if ((rc == 0) ||
                    (((rc & 1) != 0) && ((rc & 4) == 0))) {
                        fs_is_clean = TRUE;
                }

                dbus_g_method_return (context, fs_is_clean);
        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error fsck'ing: fsck exited with exit code %d: %s",
                                     WEXITSTATUS (status),
                                     stderr);
                }
        }
}

static void
devkit_disks_device_filesystem_check_authorized_cb (DevkitDisksDaemon     *daemon,
                                                    DevkitDisksDevice     *device,
                                                    DBusGMethodInvocation *context,
                                                    const gchar           *action_id,
                                                    guint                  num_user_data,
                                                    gpointer              *user_data_elements)
{
        /* TODO: use options! */
        //gchar **options = user_data_elements[0];
        int n;
        char *argv[16];
        GError *error;

        /* TODO: change when we have a file system that supports online fsck */
        if (device->priv->device_is_mounted) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_BUSY,
                             "Device is mounted and no online capability in fsck tool for file system");
                goto out;
        }

        n = 0;
        argv[n++] = "fsck";
        argv[n++] = "-a";
        argv[n++] = device->priv->device_file;
        argv[n++] = NULL;

        error = NULL;
        if (!job_new (context,
                      "FilesystemCheck",
                      FALSE,
                      device,
                      argv,
                      NULL,
                      filesystem_check_completed_cb,
                      NULL,
                      NULL)) {
                goto out;
        }

out:
        ;
}

gboolean
devkit_disks_device_filesystem_check (DevkitDisksDevice     *device,
                                      char                 **options,
                                      DBusGMethodInvocation *context)
{
        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              device->priv->device_is_system_internal ?
                                               "org.freedesktop.devicekit.disks.filesystem-check-system-internal" :
                                               "org.freedesktop.devicekit.disks.filesystem-check",
                                              "FilesystemCheck",
                                              TRUE,
                                              devkit_disks_device_filesystem_check_authorized_cb,
                                              context,
                                              1,
                                              g_strdupv (options), g_strfreev);
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
partition_delete_completed_cb (DBusGMethodInvocation *context,
                               DevkitDisksDevice *device,
                               gboolean job_was_cancelled,
                               int status,
                               const char *stderr,
                               const char *stdout,
                               gpointer user_data)
{
        DevkitDisksDevice *enclosing_device = DEVKIT_DISKS_DEVICE (user_data);

        /* poke the kernel about the enclosing disk so we can reread the partitioning table */
        devkit_disks_device_generate_kernel_change_event (enclosing_device);

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {
                dbus_g_method_return (context);
        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error erasing: helper exited with exit code %d: %s",
                                     WEXITSTATUS (status),
                                     stderr);
                }
        }
}

static void
devkit_disks_device_partition_delete_authorized_cb (DevkitDisksDaemon     *daemon,
                                                    DevkitDisksDevice     *device,
                                                    DBusGMethodInvocation *context,
                                                    const gchar           *action_id,
                                                    guint                  num_user_data,
                                                    gpointer              *user_data_elements)
{
        gchar **options = user_data_elements[0];
        int n;
        int m;
        char *argv[16];
        GError *error;
        char *offset_as_string;
        char *size_as_string;
        char *part_number_as_string;
        DevkitDisksDevice *enclosing_device;

        offset_as_string = NULL;
        size_as_string = NULL;
        part_number_as_string = NULL;
        error = NULL;

        if (devkit_disks_device_local_is_busy (device, FALSE, &error)) {
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        if (!device->priv->device_is_partition) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a partition");
                goto out;
        }

        enclosing_device = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon,
                                                                          device->priv->partition_slave);
        if (enclosing_device == NULL) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Cannot find enclosing device");
                goto out;
        }

        if (devkit_disks_device_local_is_busy (enclosing_device, FALSE, &error)) {
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        offset_as_string = g_strdup_printf ("%" G_GINT64_FORMAT "", device->priv->partition_offset);
        size_as_string = g_strdup_printf ("%" G_GINT64_FORMAT "", device->priv->partition_size);
        part_number_as_string = g_strdup_printf ("%d", device->priv->partition_number);

        n = 0;
        argv[n++] = PACKAGE_LIBEXEC_DIR "/devkit-disks-helper-delete-partition";
        argv[n++] = enclosing_device->priv->device_file;
        argv[n++] = device->priv->device_file;
        argv[n++] = offset_as_string;
        argv[n++] = size_as_string;
        argv[n++] = part_number_as_string;
        for (m = 0; options[m] != NULL; m++) {
                if (n >= (int) sizeof (argv) - 1) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Too many options");
                        goto out;
                }
                /* the helper will validate each option */
                argv[n++] = (char *) options[m];
        }
        argv[n++] = NULL;

        if (!job_new (context,
                      "PartitionDelete",
                      TRUE,
                      device,
                      argv,
                      NULL,
                      partition_delete_completed_cb,
                      g_object_ref (enclosing_device),
                      g_object_unref)) {
                goto out;
        }

out:
        g_free (offset_as_string);
        g_free (size_as_string);
        g_free (part_number_as_string);
}

gboolean
devkit_disks_device_partition_delete (DevkitDisksDevice     *device,
                                      char                 **options,
                                      DBusGMethodInvocation *context)
{
        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              device->priv->device_is_system_internal ?
                                               "org.freedesktop.devicekit.disks.change-system-internal" :
                                               "org.freedesktop.devicekit.disks.change",
                                              "PartitionDelete",
                                              TRUE,
                                              devkit_disks_device_partition_delete_authorized_cb,
                                              context,
                                              1,
                                              g_strdupv (options), g_strfreev);
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

typedef struct {
        FilesystemCreateHookFunc hook_func;
        gpointer                 hook_user_data;
} MkfsData;

static void
mkfs_data_unref (MkfsData *data)
{
        g_free (data);
}

static void
filesystem_create_completed_cb (DBusGMethodInvocation *context,
                                DevkitDisksDevice *device,
                                gboolean job_was_cancelled,
                                int status,
                                const char *stderr,
                                const char *stdout,
                                gpointer user_data)
{
        MkfsData *data = user_data;

        /* poke the kernel so we can reread the data */
        devkit_disks_device_generate_kernel_change_event (device);

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {
                if (data->hook_func != NULL)
                        data->hook_func (context, device, TRUE, data->hook_user_data);
                else
                        dbus_g_method_return (context);
        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error creating file system: helper exited with exit code %d: %s",
                                     WEXITSTATUS (status),
                                     stderr);
                }

                if (data->hook_func != NULL)
                        data->hook_func (context, device, FALSE, data->hook_user_data);
        }
}

typedef struct {
        int refcount;

        DBusGMethodInvocation *context;
        DevkitDisksDevice *device;

        char *passphrase;

        char **options;
        char *fstype;

        FilesystemCreateHookFunc mkfs_hook_func;
        gpointer                 mkfs_hook_user_data;

        guint device_changed_signal_handler_id;
        guint device_changed_timeout_id;
} MkfsLuksData;

static MkfsLuksData *
mkfse_data_ref (MkfsLuksData *data)
{
        data->refcount++;
        return data;
}

static void
mkfse_data_unref (MkfsLuksData *data)
{
        data->refcount--;
        if (data->refcount == 0) {
                if (data->passphrase != NULL) {
                        memset (data->passphrase, '\0', strlen (data->passphrase));
                        g_free (data->passphrase);
                }
                if (data->device != NULL)
                        g_object_unref (data->device);
                g_strfreev (data->options);
                g_free (data->fstype);
                g_free (data);
        }
}

static void
filesystem_create_wait_for_cleartext_device_hook (DBusGMethodInvocation *context,
                                                  DevkitDisksDevice *device,
                                                  gpointer user_data)
{
        MkfsLuksData *data = user_data;

        if (device == NULL) {
                /* Dang, unlocking failed. The unlock method have already thrown an exception for us. */
        } else {
                /* We're unlocked.. awesome.. Now we can _finally_ create the file system.
                 * What a ride. We're returning to exactly to where we came from. Back to
                 * the source. Only the device is different.
                 */

                devkit_disks_device_filesystem_create_internal (device,
                                                                data->fstype,
                                                                data->options,
                                                                data->mkfs_hook_func,
                                                                data->mkfs_hook_user_data,
                                                                data->context);
                mkfse_data_unref (data);
        }
}

static void
filesystem_create_wait_for_luks_device_changed_cb (DevkitDisksDaemon *daemon,
                                                        const char *object_path,
                                                        gpointer user_data)
{
        MkfsLuksData *data = user_data;
        DevkitDisksDevice *device;

        /* check if we're now a LUKS crypto device */
        device = devkit_disks_daemon_local_find_by_object_path (daemon, object_path);
        if (device == data->device &&
            (device->priv->id_usage != NULL && strcmp (device->priv->id_usage, "crypto") == 0) &&
            (device->priv->id_type != NULL && strcmp (device->priv->id_type, "crypto_LUKS") == 0)) {

                /* yay! we are now set up the corresponding cleartext device */

                devkit_disks_device_luks_unlock_internal (data->device,
                                                               data->passphrase,
                                                               NULL,
                                                               filesystem_create_wait_for_cleartext_device_hook,
                                                               data,
                                                               data->context);

                g_signal_handler_disconnect (daemon, data->device_changed_signal_handler_id);
                g_source_remove (data->device_changed_timeout_id);
        }
}

static gboolean
filesystem_create_wait_for_luks_device_not_seen_cb (gpointer user_data)
{
        MkfsLuksData *data = user_data;

        throw_error (data->context,
                     DEVKIT_DISKS_ERROR_FAILED,
                     "Error creating luks encrypted file system: timeout (10s) waiting for luks device to show up");

        g_signal_handler_disconnect (data->device->priv->daemon, data->device_changed_signal_handler_id);
        mkfse_data_unref (data);

        return FALSE;
}



static void
filesystem_create_create_luks_device_completed_cb (DBusGMethodInvocation *context,
                                                        DevkitDisksDevice *device,
                                                        gboolean job_was_cancelled,
                                                        int status,
                                                        const char *stderr,
                                                        const char *stdout,
                                                        gpointer user_data)
{
        MkfsLuksData *data = user_data;

        /* poke the kernel so we can reread the data (new uuid etc.) */
        devkit_disks_device_generate_kernel_change_event (device);

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {

                /* OK! So we've got ourselves an luks device. Let's set it up so we can create a file
                 * system. Sit and wait for the change event to appear so we can setup with the right UUID.
                 */

                data->device_changed_signal_handler_id = g_signal_connect_after (
                        device->priv->daemon,
                        "device-changed",
                        (GCallback) filesystem_create_wait_for_luks_device_changed_cb,
                        mkfse_data_ref (data));

                /* set up timeout for error reporting if waiting failed
                 *
                 * (the signal handler and the timeout handler share the ref to data
                 * as one will cancel the other)
                 */
                data->device_changed_timeout_id = g_timeout_add (
                        10 * 1000,
                        filesystem_create_wait_for_luks_device_not_seen_cb,
                        data);


        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error creating file system: cryptsetup exited with exit code %d: %s",
                                     WEXITSTATUS (status),
                                     stderr);
                }
        }
}

static gboolean
devkit_disks_device_filesystem_create_internal (DevkitDisksDevice       *device,
                                                const char              *fstype,
                                                char                   **options,
                                                FilesystemCreateHookFunc hook_func,
                                                gpointer                 hook_user_data,
                                                DBusGMethodInvocation *context)
{
        int n, m;
        char *argv[128];
        GError *error;
        char *s;
        char *options_for_stdin;
        char *passphrase_stdin;
        MkfsData *mkfs_data;

        options_for_stdin = NULL;
        passphrase_stdin = NULL;
        error = NULL;

        if (devkit_disks_device_local_is_busy (device, TRUE, &error)) {
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        if (strlen (fstype) == 0) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "fstype not specified");
                goto out;
        }

        /* search for luks_encrypt=<passphrase> and do a detour if that's specified */
        for (n = 0; options[n] != NULL; n++) {
                if (g_str_has_prefix (options[n], "luks_encrypt=")) {
                        MkfsLuksData *mkfse_data;

                        /* So this is a request to create an luks device to put the
                         * file system on; save all options for mkfs (except luks_encrypt=) for
                         * later invocation once we have a cleartext device.
                         */

                        mkfse_data = g_new0 (MkfsLuksData, 1);
                        mkfse_data->refcount = 1;
                        mkfse_data->context = context;
                        mkfse_data->device = g_object_ref (device);
                        mkfse_data->passphrase = g_strdup (options[n] + sizeof ("luks_encrypt=") - 1);
                        mkfse_data->mkfs_hook_func = hook_func;
                        mkfse_data->mkfs_hook_user_data = hook_user_data;
                        mkfse_data->fstype = g_strdup (fstype);
                        mkfse_data->options = g_strdupv (options);
                        g_free (mkfse_data->options[n]);
                        for (m = n; mkfse_data->options[m] != NULL; m++) {
                                mkfse_data->options[m] = mkfse_data->options[m + 1];
                        }

                        passphrase_stdin = g_strdup_printf ("%s\n", mkfse_data->passphrase);

                        n = 0;
                        argv[n++] = "cryptsetup";
                        argv[n++] = "-q";
                        argv[n++] = "luksFormat";
                        argv[n++] = device->priv->device_file;
                        argv[n++] = NULL;

                        error = NULL;
                        if (!job_new (context,
                                      "LuksFormat",
                                      TRUE,
                                      device,
                                      argv,
                                      passphrase_stdin,
                                      filesystem_create_create_luks_device_completed_cb,
                                      mkfse_data,
                                      (GDestroyNotify) mkfse_data_unref)) {
                                goto out;
                        }

                        goto out;
                }
        }

        mkfs_data = g_new (MkfsData, 1);
        mkfs_data->hook_func = hook_func;
        mkfs_data->hook_user_data = hook_user_data;

        /* pass options on stdin as it may contain secrets */
        s = g_strjoinv ("\n", options);
        options_for_stdin = g_strconcat (s, "\n\n", NULL);
        g_free (s);

        n = 0;
        argv[n++] = PACKAGE_LIBEXEC_DIR "/devkit-disks-helper-mkfs";
        argv[n++] = (char *) fstype;
        argv[n++] = device->priv->device_file;
        argv[n++] = device->priv->device_is_partition_table ? "1" : "0";
        argv[n++] = NULL;

        if (!job_new (context,
                      "FilesystemCreate",
                      TRUE,
                      device,
                      argv,
                      options_for_stdin,
                      filesystem_create_completed_cb,
                      mkfs_data,
                      (GDestroyNotify) mkfs_data_unref)) {
                goto out;
        }

out:
        g_free (options_for_stdin);
        if (passphrase_stdin != NULL) {
                memset (passphrase_stdin, '\0', strlen (passphrase_stdin));
                g_free (passphrase_stdin);
        }
        return TRUE;
}

static void
devkit_disks_device_filesystem_create_authorized_cb (DevkitDisksDaemon     *daemon,
                                                     DevkitDisksDevice     *device,
                                                     DBusGMethodInvocation *context,
                                                     const gchar           *action_id,
                                                     guint                  num_user_data,
                                                     gpointer              *user_data_elements)
{
        const gchar *fstype = user_data_elements[0];
        gchar **options = user_data_elements[1];
        devkit_disks_device_filesystem_create_internal (device, fstype, options, NULL, NULL, context);
}

gboolean
devkit_disks_device_filesystem_create (DevkitDisksDevice     *device,
                                       const char            *fstype,
                                       char                 **options,
                                       DBusGMethodInvocation *context)
{
        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              device->priv->device_is_system_internal ?
                                                "org.freedesktop.devicekit.disks.change-system-internal" :
                                                "org.freedesktop.devicekit.disks.change",
                                              "FilesystemCreate",
                                              TRUE,
                                              devkit_disks_device_filesystem_create_authorized_cb,
                                              context,
                                              2,
                                              g_strdup (fstype), g_free,
                                              g_strdupv (options), g_strfreev);
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
devkit_disks_device_job_cancel_authorized_cb (DevkitDisksDaemon     *daemon,
                                              DevkitDisksDevice     *device,
                                              DBusGMethodInvocation *context,
                                              const gchar           *action_id,
                                              guint                  num_user_data,
                                              gpointer              *user_data_elements)
{

        if (!device->priv->job_in_progress) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "There is no job to cancel");
                goto out;
        }

        if (!device->priv->job_is_cancellable) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Job cannot be cancelled");
                goto out;
        }

        job_cancel (device);

        /* TODO: wait returning once the job is actually cancelled? */
        dbus_g_method_return (context);
 out:
        ;
}

gboolean
devkit_disks_device_job_cancel (DevkitDisksDevice     *device,
                                DBusGMethodInvocation *context)
{
        uid_t uid;
        const gchar *action_id;

        if (!device->priv->job_in_progress) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "There is no job to cancel");
                goto out;
        }

        if (!device->priv->job_is_cancellable) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Job cannot be cancelled");
                goto out;
        }

        devkit_disks_daemon_local_get_uid (device->priv->daemon, &uid, context);

        action_id = NULL;
        if (device->priv->job_initiated_by_uid != uid) {
                action_id = "org.freedesktop.devicekit.disks.cancel-job-others";
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              action_id,
                                              "JobCancel",
                                              TRUE,
                                              devkit_disks_device_job_cancel_authorized_cb,
                                              context,
                                              0);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

typedef struct {
        int refcount;

        guint device_added_signal_handler_id;
        guint device_added_timeout_id;

        DBusGMethodInvocation *context;
        DevkitDisksDevice *device;
        guint64 offset;
        guint64 size;

        guint64 created_offset;
        guint64 created_size;

        char *fstype;
        char **fsoptions;

} CreatePartitionData;

static CreatePartitionData *
partition_create_data_new (DBusGMethodInvocation *context,
                           DevkitDisksDevice *device,
                           guint64 offset,
                           guint64 size,
                           const char *fstype,
                           char **fsoptions)
{
        CreatePartitionData *data;

        data = g_new0 (CreatePartitionData, 1);
        data->refcount = 1;

        data->context = context;
        data->device = g_object_ref (device);
        data->offset = offset;
        data->size = size;
        data->fstype = g_strdup (fstype);
        data->fsoptions = g_strdupv (fsoptions);

        return data;
}

static CreatePartitionData *
partition_create_data_ref (CreatePartitionData *data)
{
        data->refcount++;
        return data;
}

static void
partition_create_data_unref (CreatePartitionData *data)
{
        data->refcount--;
        if (data->refcount == 0) {
                g_object_unref (data->device);
                g_free (data->fstype);
                g_strfreev (data->fsoptions);
                g_free (data);
        }
}

static void
partition_create_filesystem_create_hook (DBusGMethodInvocation *context,
                                         DevkitDisksDevice *device,
                                         gboolean filesystem_create_succeeded,
                                         gpointer user_data)
{
        if (!filesystem_create_succeeded) {
                /* dang.. FilesystemCreate already reported an error */
        } else {
                /* it worked.. */
                dbus_g_method_return (context, device->priv->object_path);
        }
}

static void
partition_create_device_added_cb (DevkitDisksDaemon *daemon,
                                  const char *object_path,
                                  gpointer user_data)
{
        CreatePartitionData *data = user_data;
        DevkitDisksDevice *device;

        /* check the device added is the partition we've created */
        device = devkit_disks_daemon_local_find_by_object_path (daemon, object_path);
        if (device != NULL &&
            device->priv->device_is_partition &&
            strcmp (device->priv->partition_slave, data->device->priv->object_path) == 0 &&
            data->created_offset == device->priv->partition_offset &&
            data->created_size == device->priv->partition_size) {

                /* yay! it is.. now create the file system if requested */
                if (strlen (data->fstype) > 0) {
                        devkit_disks_device_filesystem_create_internal (device,
                                                                        data->fstype,
                                                                        data->fsoptions,
                                                                        partition_create_filesystem_create_hook,
                                                                        NULL,
                                                                        data->context);
                } else {
                        dbus_g_method_return (data->context, device->priv->object_path);
                }

                g_signal_handler_disconnect (daemon, data->device_added_signal_handler_id);
                g_source_remove (data->device_added_timeout_id);
                partition_create_data_unref (data);
        }
}

static gboolean
partition_create_device_not_seen_cb (gpointer user_data)
{
        CreatePartitionData *data = user_data;

        throw_error (data->context,
                     DEVKIT_DISKS_ERROR_FAILED,
                     "Error creating partition: timeout (10s) waiting for partition to show up");

        g_signal_handler_disconnect (data->device->priv->daemon, data->device_added_signal_handler_id);
        partition_create_data_unref (data);

        return FALSE;
}

static void
partition_create_completed_cb (DBusGMethodInvocation *context,
                               DevkitDisksDevice *device,
                               gboolean job_was_cancelled,
                               int status,
                               const char *stderr,
                               const char *stdout,
                               gpointer user_data)
{
        CreatePartitionData *data = user_data;

        /* poke the kernel so we can reread the data */
        devkit_disks_device_generate_kernel_change_event (device);

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {
                int n;
                int m;
                guint64 offset;
                guint64 size;
                char **tokens;

                /* Find the
                 *
                 *   job-create-partition-offset:
                 *   job-create-partition-size:
                 *
                 * lines and parse the new start and end. We need this
                 * for waiting on the created partition (since the requested
                 * start and size passed may not be honored due to disk/cylinder/sector
                 * alignment reasons).
                 */
                offset = 0;
                size = 0;
                m = 0;
                tokens = g_strsplit (stderr, "\n", 0);
                for (n = 0; tokens[n] != NULL; n++) {
                        char *line = tokens[n];
                        char *endp;

                        if (m == 2)
                                break;

                        if (g_str_has_prefix (line, "job-create-partition-offset: ")) {
                                offset = strtoll (line + sizeof ("job-create-partition-offset: ") - 1, &endp, 10);
                                if (*endp == '\0')
                                        m++;
                        } else if (g_str_has_prefix (line, "job-create-partition-size: ")) {
                                size = strtoll (line + sizeof ("job-create-partition-size: ") - 1, &endp, 10);
                                if (*endp == '\0')
                                        m++;
                        }
                }
                g_strfreev (tokens);

                if (m != 2) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error creating partition: internal error, expected to find new "
                                     "start and end but m=%d", m);
                } else {
                        data->created_offset = offset;
                        data->created_size = size;

                        /* sit around and wait for the new partition to appear */
                        data->device_added_signal_handler_id = g_signal_connect_after (
                                device->priv->daemon,
                                "device-added",
                                (GCallback) partition_create_device_added_cb,
                                partition_create_data_ref (data));

                        /* set up timeout for error reporting if waiting failed
                         *
                         * (the signal handler and the timeout handler share the ref to data
                         * as one will cancel the other)
                         */
                        data->device_added_timeout_id = g_timeout_add (10 * 1000,
                                                                       partition_create_device_not_seen_cb,
                                                                       data);
                }
        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error creating partition: helper exited with exit code %d: %s",
                                     WEXITSTATUS (status),
                                     stderr);
                }
        }
}

static void
devkit_disks_device_partition_create_authorized_cb (DevkitDisksDaemon     *daemon,
                                                    DevkitDisksDevice     *device,
                                                    DBusGMethodInvocation *context,
                                                    const gchar           *action_id,
                                                    guint                  num_user_data,
                                                    gpointer              *user_data_elements)
{
        guint64                offset    = *((guint64*) user_data_elements[0]);
        guint64                size      = *((guint64*) user_data_elements[1]);;
        const char            *type      = user_data_elements[2];
        const char            *label     = user_data_elements[3];
        char                 **flags     = user_data_elements[4];
        char                 **options   = user_data_elements[5];
        const char            *fstype    = user_data_elements[6];
        char                 **fsoptions = user_data_elements[7];
        int n;
        int m;
        char *argv[128];
        GError *error;
        char *offset_as_string;
        char *size_as_string;
        char *flags_as_string;

        offset_as_string = NULL;
        size_as_string = NULL;
        flags_as_string = NULL;
        error = NULL;

        if (!device->priv->device_is_partition_table) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not partitioned");
                goto out;
        }

        if (devkit_disks_device_local_is_busy (device, FALSE, &error)) {
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        /* partutil.c / libparted will check there are no partitions in the requested slice */

        offset_as_string = g_strdup_printf ("%" G_GINT64_FORMAT "", offset);
        size_as_string = g_strdup_printf ("%" G_GINT64_FORMAT "", size);
        /* TODO: check that neither of the flags include ',' */
        flags_as_string = g_strjoinv (",", flags);

        n = 0;
        argv[n++] = PACKAGE_LIBEXEC_DIR "/devkit-disks-helper-create-partition";
        argv[n++] = device->priv->device_file;;
        argv[n++] = offset_as_string;
        argv[n++] = size_as_string;
        argv[n++] = (char *) type;
        argv[n++] = (char *) label;
        argv[n++] = (char *) flags_as_string;
        for (m = 0; options[m] != NULL; m++) {
                if (n >= (int) sizeof (argv) - 1) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Too many options");
                        goto out;
                }
                /* the helper will validate each option */
                argv[n++] = (char *) options[m];
        }
        argv[n++] = NULL;

        if (!job_new (context,
                      "PartitionCreate",
                      TRUE,
                      device,
                      argv,
                      NULL,
                      partition_create_completed_cb,
                      partition_create_data_new (context, device, offset, size, fstype, fsoptions),
                      (GDestroyNotify) partition_create_data_unref)) {
                goto out;
        }

out:
        g_free (offset_as_string);
        g_free (size_as_string);
        g_free (flags_as_string);
}

gboolean
devkit_disks_device_partition_create (DevkitDisksDevice     *device,
                                      guint64                offset,
                                      guint64                size,
                                      const char            *type,
                                      const char            *label,
                                      char                 **flags,
                                      char                 **options,
                                      const char            *fstype,
                                      char                 **fsoptions,
                                      DBusGMethodInvocation *context)
{
        if (!device->priv->device_is_partition_table) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not partitioned");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              device->priv->device_is_system_internal ?
                                                "org.freedesktop.devicekit.disks.change-system-internal" :
                                                "org.freedesktop.devicekit.disks.change",
                                              "PartitionCreate",
                                              TRUE,
                                              devkit_disks_device_partition_create_authorized_cb,
                                              context,
                                              8,
                                              g_memdup (&offset, sizeof (guint64)), g_free,
                                              g_memdup (&size, sizeof (guint64)), g_free,
                                              g_strdup (type), g_free,
                                              g_strdup (label), g_free,
                                              g_strdupv (flags), g_strfreev,
                                              g_strdupv (options), g_strfreev,
                                              g_strdup (fstype), g_free,
                                              g_strdupv (fsoptions), g_strfreev);

 out:
        return TRUE;
}


/*--------------------------------------------------------------------------------------------------------------*/

typedef struct {
        DBusGMethodInvocation *context;
        DevkitDisksDevice *device;
        DevkitDisksDevice *enclosing_device;

        char *type;
        char *label;
        char **flags;
} ModifyPartitionData;

static ModifyPartitionData *
partition_modify_data_new (DBusGMethodInvocation *context,
                           DevkitDisksDevice *device,
                           DevkitDisksDevice *enclosing_device,
                           const char *type,
                           const char *label,
                           char **flags)
{
        ModifyPartitionData *data;

        data = g_new0 (ModifyPartitionData, 1);

        data->context = context;
        data->device = g_object_ref (device);
        data->enclosing_device = g_object_ref (enclosing_device);
        data->type = g_strdup (type);
        data->label = g_strdup (label);
        data->flags = g_strdupv (flags);

        return data;
}

static void
partition_modify_data_unref (ModifyPartitionData *data)
{
        g_object_unref (data->device);
        g_object_unref (data->enclosing_device);
        g_free (data->type);
        g_free (data->label);
        g_free (data->flags);
        g_free (data);
}

static void
partition_modify_completed_cb (DBusGMethodInvocation *context,
                               DevkitDisksDevice *device,
                               gboolean job_was_cancelled,
                               int status,
                               const char *stderr,
                               const char *stdout,
                               gpointer user_data)
{
        ModifyPartitionData *data = user_data;

        /* poke the kernel so we can reread the data */
        devkit_disks_device_generate_kernel_change_event (data->enclosing_device);
        devkit_disks_device_generate_kernel_change_event (data->device);

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {
                /* update local copy, don't wait for the kernel */

                devkit_disks_device_set_partition_type (device, data->type);
                devkit_disks_device_set_partition_label (device, data->label);
                devkit_disks_device_set_partition_flags (device, data->flags);

                drain_pending_changes (device, FALSE);

                dbus_g_method_return (context);

        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error modifying partition: helper exited with exit code %d: %s",
                                     WEXITSTATUS (status),
                                     stderr);
                }
        }
}

static void
devkit_disks_device_partition_modify_authorized_cb (DevkitDisksDaemon     *daemon,
                                                    DevkitDisksDevice     *device,
                                                    DBusGMethodInvocation *context,
                                                    const gchar           *action_id,
                                                    guint                  num_user_data,
                                                    gpointer              *user_data_elements)
{
        const char            *type = user_data_elements[0];
        const char            *label = user_data_elements[1];
        char                 **flags = user_data_elements[2];
        int n;
        char *argv[128];
        GError *error;
        char *offset_as_string;
        char *size_as_string;
        char *flags_as_string;
        DevkitDisksDevice *enclosing_device;

        offset_as_string = NULL;
        size_as_string = NULL;
        flags_as_string = NULL;
        error = NULL;

        if (!device->priv->device_is_partition) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a partition");
                goto out;
        }

        enclosing_device = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon,
                                                                          device->priv->partition_slave);
        if (enclosing_device == NULL) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Cannot find enclosing device");
                goto out;
        }

        if (devkit_disks_device_local_is_busy (enclosing_device, FALSE, &error)) {
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        if (strlen (type) == 0) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "type not specified");
                goto out;
        }

        offset_as_string = g_strdup_printf ("%" G_GINT64_FORMAT "", device->priv->partition_offset);
        size_as_string = g_strdup_printf ("%" G_GINT64_FORMAT "", device->priv->partition_size);
        /* TODO: check that neither of the flags include ',' */
        flags_as_string = g_strjoinv (",", flags);

        n = 0;
        argv[n++] = PACKAGE_LIBEXEC_DIR "/devkit-disks-helper-modify-partition";
        argv[n++] = enclosing_device->priv->device_file;
        argv[n++] = offset_as_string;
        argv[n++] = size_as_string;
        argv[n++] = (char *) type;
        argv[n++] = (char *) label;
        argv[n++] = (char *) flags_as_string;
        argv[n++] = NULL;

        if (!job_new (context,
                      "PartitionModify",
                      TRUE,
                      device,
                      argv,
                      NULL,
                      partition_modify_completed_cb,
                      partition_modify_data_new (context, device, enclosing_device, type, label, flags),
                      (GDestroyNotify) partition_modify_data_unref)) {
                goto out;
        }

out:
        g_free (offset_as_string);
        g_free (size_as_string);
        g_free (flags_as_string);
}

gboolean
devkit_disks_device_partition_modify (DevkitDisksDevice     *device,
                                      const char            *type,
                                      const char            *label,
                                      char                 **flags,
                                      DBusGMethodInvocation *context)
{
        if (!device->priv->device_is_partition) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a partition");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              device->priv->device_is_system_internal ?
                                                "org.freedesktop.devicekit.disks.change-system-internal" :
                                                "org.freedesktop.devicekit.disks.change",
                                              "PartitionModify",
                                              TRUE,
                                              devkit_disks_device_partition_modify_authorized_cb,
                                              context,
                                              3,
                                              g_strdup (type), g_free,
                                              g_strdup (label), g_free,
                                              g_strdupv (flags), g_strfreev);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

typedef struct {
        int refcount;

        guint device_changed_signal_handler_id;
        guint device_changed_timeout_id;

        DBusGMethodInvocation *context;
        DevkitDisksDevice *device;

        char *scheme;

} CreatePartitionTableData;

static CreatePartitionTableData *
partition_table_create_data_new (DBusGMethodInvocation *context,
                                 DevkitDisksDevice *device,
                                 const char *scheme)
{
        CreatePartitionTableData *data;

        data = g_new0 (CreatePartitionTableData, 1);
        data->refcount = 1;

        data->context = context;
        data->device = g_object_ref (device);
        data->scheme = g_strdup (scheme);

        return data;
}

static CreatePartitionTableData *
partition_table_create_data_ref (CreatePartitionTableData *data)
{
        data->refcount++;
        return data;
}

static void
partition_table_create_data_unref (CreatePartitionTableData *data)
{
        data->refcount--;
        if (data->refcount == 0) {
                g_object_unref (data->device);
                g_free (data->scheme);
                g_free (data);
        }
}

static void
partition_table_create_device_changed_cb (DevkitDisksDaemon *daemon,
                                          const char *object_path,
                                          gpointer user_data)
{
        CreatePartitionTableData *data = user_data;
        DevkitDisksDevice *device;

        device = devkit_disks_daemon_local_find_by_object_path (daemon, object_path);
        if (device == data->device) {
                if (g_strcmp0 (device->priv->partition_table_scheme, data->scheme) == 0 ||
                    (device->priv->partition_table_scheme == NULL && g_strcmp0 (data->scheme, "none") == 0)) {
                        dbus_g_method_return (data->context);

                        g_signal_handler_disconnect (daemon, data->device_changed_signal_handler_id);
                        g_source_remove (data->device_changed_timeout_id);
                        partition_table_create_data_unref (data);
                }
        }
}

static gboolean
partition_table_create_device_not_changed_cb (gpointer user_data)
{
        CreatePartitionTableData *data = user_data;

        throw_error (data->context,
                     DEVKIT_DISKS_ERROR_FAILED,
                     "Error creating partition table: timeout (10s) waiting for change");

        g_signal_handler_disconnect (data->device->priv->daemon, data->device_changed_signal_handler_id);
        partition_table_create_data_unref (data);

        return FALSE;
}

static void
partition_table_create_completed_cb (DBusGMethodInvocation *context,
                                     DevkitDisksDevice *device,
                                     gboolean job_was_cancelled,
                                     int status,
                                     const char *stderr,
                                     const char *stdout,
                                     gpointer user_data)
{
        CreatePartitionTableData *data = user_data;

        /* poke the kernel so we can reread the data */
        devkit_disks_device_generate_kernel_change_event (device);

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {

                if (g_strcmp0 (device->priv->partition_table_scheme, data->scheme) == 0) {
                        dbus_g_method_return (context);
                } else {
                        /* sit around and wait for the new partition table to appear */
                        data->device_changed_signal_handler_id = g_signal_connect_after (
                                device->priv->daemon,
                                "device-changed",
                                (GCallback) partition_table_create_device_changed_cb,
                                partition_table_create_data_ref (data));

                        /* set up timeout for error reporting if waiting failed
                         *
                         * (the signal handler and the timeout handler share the ref to data
                         * as one will cancel the other)
                         */
                        data->device_changed_timeout_id = g_timeout_add (10 * 1000,
                                                                         partition_table_create_device_not_changed_cb,
                                                                         data);
                }

        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error creating partition table: helper exited with exit code %d: %s",
                                     WEXITSTATUS (status),
                                     stderr);
                }
        }
}

static void
devkit_disks_device_partition_table_create_authorized_cb (DevkitDisksDaemon     *daemon,
                                                          DevkitDisksDevice     *device,
                                                          DBusGMethodInvocation *context,
                                                          const gchar           *action_id,
                                                          guint                  num_user_data,
                                                          gpointer              *user_data_elements)
{
        const char            *scheme  = user_data_elements[0];
        char                 **options = user_data_elements[1];
        int n;
        int m;
        char *argv[128];
        GError *error;

        error = NULL;

        if (devkit_disks_device_local_is_busy (device, TRUE, &error)) {
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        if (strlen (scheme) == 0) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "type not specified");
                goto out;
        }

        if (g_strcmp0 (device->priv->partition_table_scheme, scheme) == 0) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "device already has a partition table of given scheme");
                goto out;
        }

        n = 0;
        argv[n++] = PACKAGE_LIBEXEC_DIR "/devkit-disks-helper-create-partition-table";
        argv[n++] = device->priv->device_file;
        argv[n++] = (char *) scheme;
        for (m = 0; options[m] != NULL; m++) {
                if (n >= (int) sizeof (argv) - 1) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Too many options");
                        goto out;
                }
                /* the helper will validate each option */
                argv[n++] = (char *) options[m];
        }
        argv[n++] = NULL;

        if (!job_new (context,
                      "PartitionTableCreate",
                      TRUE,
                      device,
                      argv,
                      NULL,
                      partition_table_create_completed_cb,
                      partition_table_create_data_new (context, device, scheme),
                      (GDestroyNotify) partition_table_create_data_unref)) {
                goto out;
        }

out:
        ;
}

gboolean
devkit_disks_device_partition_table_create (DevkitDisksDevice     *device,
                                            const char            *scheme,
                                            char                 **options,
                                            DBusGMethodInvocation *context)
{
        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              device->priv->device_is_system_internal ?
                                                "org.freedesktop.devicekit.disks.change-system-internal" :
                                                "org.freedesktop.devicekit.disks.change",
                                              "PartitionTableCreate",
                                              TRUE,
                                              devkit_disks_device_partition_table_create_authorized_cb,
                                              context,
                                              2,
                                              g_strdup (scheme), g_free,
                                              g_strdupv (options), g_strfreev);
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static DevkitDisksDevice *
find_cleartext_device (DevkitDisksDevice *device)
{
        GList *devices;
        GList *l;
        DevkitDisksDevice *ret;

        ret = NULL;

        /* check that there isn't a cleartext device already  */
        devices = devkit_disks_daemon_local_get_all_devices (device->priv->daemon);
        for (l = devices; l != NULL; l = l->next) {
                DevkitDisksDevice *d = DEVKIT_DISKS_DEVICE (l->data);
                if (d->priv->device_is_luks_cleartext &&
                    d->priv->luks_cleartext_slave != NULL &&
                    strcmp (d->priv->luks_cleartext_slave, device->priv->object_path) == 0) {
                        ret = d;
                        goto out;
                }
        }

out:

        g_list_free (devices);

        return ret;
}

typedef struct {
        int refcount;

        gulong device_added_signal_handler_id;
        guint device_added_timeout_id;

        DBusGMethodInvocation *context;
        DevkitDisksDevice *device;

        UnlockEncryptionHookFunc    hook_func;
        gpointer                    hook_user_data;
} UnlockEncryptionData;

static UnlockEncryptionData *
unlock_encryption_data_new (DBusGMethodInvocation      *context,
                            DevkitDisksDevice          *device,
                            UnlockEncryptionHookFunc    hook_func,
                            gpointer                    hook_user_data)
{
        UnlockEncryptionData *data;

        data = g_new0 (UnlockEncryptionData, 1);
        data->refcount = 1;

        data->context = context;
        data->device = g_object_ref (device);
        data->hook_func = hook_func;
        data->hook_user_data = hook_user_data;
        return data;
}

static UnlockEncryptionData *
unlock_encryption_data_ref (UnlockEncryptionData *data)
{
        data->refcount++;
        return data;
}

static void
unlock_encryption_data_unref (UnlockEncryptionData *data)
{
        data->refcount--;
        if (data->refcount == 0) {
                g_object_unref (data->device);
                g_free (data);
        }
}


static void
luks_unlock_device_added_cb (DevkitDisksDaemon *daemon,
                                  const char *object_path,
                                  gpointer user_data)
{
        UnlockEncryptionData *data = user_data;
        DevkitDisksDevice *device;

        /* check the device is a cleartext partition for us */
        device = devkit_disks_daemon_local_find_by_object_path (daemon, object_path);

        if (device != NULL &&
            device->priv->device_is_luks_cleartext &&
            strcmp (device->priv->luks_cleartext_slave, data->device->priv->object_path) == 0) {

                /* update and emit a Changed() signal on the holder since the luks-holder
                 * property indicates the cleartext device
                 */
                update_info (data->device);
                drain_pending_changes (data->device, FALSE);

                if (data->hook_func != NULL) {
                        data->hook_func (data->context, device, data->hook_user_data);
                } else {
                        dbus_g_method_return (data->context, object_path);
                }

                g_signal_handler_disconnect (daemon, data->device_added_signal_handler_id);
                g_source_remove (data->device_added_timeout_id);
                unlock_encryption_data_unref (data);
        }
}

static gboolean
luks_unlock_device_not_seen_cb (gpointer user_data)
{
        UnlockEncryptionData *data = user_data;

        throw_error (data->context,
                     DEVKIT_DISKS_ERROR_FAILED,
                     "Error unlocking device: timeout (10s) waiting for cleartext device to show up");

        if (data->hook_func != NULL) {
                data->hook_func (data->context, NULL, data->hook_user_data);
        }

        if (data->device_added_signal_handler_id > 0)
                g_signal_handler_disconnect (data->device->priv->daemon, data->device_added_signal_handler_id);

        unlock_encryption_data_unref (data);
        return FALSE;
}

static gboolean
luks_unlock_start_waiting_for_cleartext_device (gpointer user_data)
{
        UnlockEncryptionData *data = user_data;
        DevkitDisksDevice *cleartext_device;

        cleartext_device = find_cleartext_device (data->device);
        if (cleartext_device != NULL) {
                /* update and emit a Changed() signal on the holder since the luks-holder
                 * property indicates the cleartext device
                 */
                update_info (data->device);
                drain_pending_changes (data->device, FALSE);

                if (data->hook_func != NULL) {
                        data->hook_func (data->context, cleartext_device, data->hook_user_data);
                } else {
                        dbus_g_method_return (data->context, cleartext_device->priv->object_path);
                }

                unlock_encryption_data_unref (data);
        } else {
                /* sit around wait for the cleartext device to appear */
                data->device_added_signal_handler_id = g_signal_connect_after (data->device->priv->daemon,
                                                                               "device-added",
                                                                               (GCallback) luks_unlock_device_added_cb,
                                                                               data);

                /* set up timeout for error reporting if waiting failed */
                data->device_added_timeout_id = g_timeout_add (10 * 1000,
                                                               luks_unlock_device_not_seen_cb,
                                                               data);

                /* Note that the signal and timeout handlers share the ref to data - one will cancel the other */
        }

        return FALSE;
}


static void
luks_unlock_completed_cb (DBusGMethodInvocation *context,
                               DevkitDisksDevice *device,
                               gboolean job_was_cancelled,
                               int status,
                               const char *stderr,
                               const char *stdout,
                               gpointer user_data)
{
        UnlockEncryptionData *data = user_data;

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {

                /* yay, so it turns out /sbin/cryptsetup returns way too early; what happens is this
                 *
                 * - invoke /sbin/cryptsetup
                 *   - temporary dm node with name temporary-cryptsetup-* appears. We ignore these,
                 *     see above
                 *   - temporary dm node removed
                 * - /sbin/cryptsetup returns with success (brings us here)
                 *   - proper dm node appears
                 *     - with the name we requested, e.g. devkit-disks-luks-uuid-%s-uid%d
                 *   - proper dm node disappears
                 *   - proper dm node reappears
                 *
                 * Obiviously /sbin/cryptsetup shouldn't return before the dm node we are
                 * looking for is really there.
                 *
                 * TODO: file a bug against /sbin/cryptsetup, probably fix it too. This probably
                 *       involves fixing device-mapper as well
                 *
                 * CURRENT WORKAROUND: Basically, we just sleep two seconds before waiting for the
                 *                     cleartext device to appear. That way we can ignore the initial
                 *                     nodes.
                 */

                g_timeout_add (2 * 1000,
                               luks_unlock_start_waiting_for_cleartext_device,
                               unlock_encryption_data_ref (data));

        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error unlocking device: cryptsetup exited with exit code %d: %s",
                                     WEXITSTATUS (status), stderr);
                }
                if (data->hook_func != NULL) {
                        data->hook_func (data->context, NULL, data->hook_user_data);
                }
        }
}

static gboolean
devkit_disks_device_luks_unlock_internal (DevkitDisksDevice        *device,
                                          const char               *secret,
                                          char                    **options,
                                          UnlockEncryptionHookFunc  hook_func,
                                          gpointer                  hook_user_data,
                                          DBusGMethodInvocation    *context)
{
        int n;
        char *argv[10];
        char *luks_name;
        GError *error;
        char *secret_as_stdin;
        uid_t uid;

        luks_name = NULL;
        secret_as_stdin = NULL;
        error = NULL;

        devkit_disks_daemon_local_get_uid (device->priv->daemon, &uid, context);

        if (devkit_disks_device_local_is_busy (device, FALSE, &error)) {
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        if (device->priv->id_usage == NULL ||
            strcmp (device->priv->id_usage, "crypto") != 0) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Not a LUKS device");
                goto out;
        }

        if (find_cleartext_device (device) != NULL) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Cleartext device is already unlocked");
                goto out;
        }

        luks_name = g_strdup_printf ("devkit-disks-luks-uuid-%s-uid%d",
                                     device->priv->id_uuid,
                                     uid);
        secret_as_stdin = g_strdup_printf ("%s\n", secret);

        n = 0;
        argv[n++] = "cryptsetup";
        argv[n++] = "luksOpen";
        argv[n++] = device->priv->device_file;
        argv[n++] = luks_name;
        argv[n++] = NULL;

        if (!job_new (context,
                      "LuksUnlock",
                      FALSE,
                      device,
                      argv,
                      secret_as_stdin,
                      luks_unlock_completed_cb,
                      unlock_encryption_data_new (context, device, hook_func, hook_user_data),
                      (GDestroyNotify) unlock_encryption_data_unref)) {
                    goto out;
        }

out:
        /* scrub the secret */
        if (secret_as_stdin != NULL) {
                memset (secret_as_stdin, '\0', strlen (secret_as_stdin));
        }
        g_free (secret_as_stdin);
        g_free (luks_name);
        return TRUE;
}

static void
devkit_disks_device_luks_unlock_authorized_cb (DevkitDisksDaemon     *daemon,
                                               DevkitDisksDevice     *device,
                                               DBusGMethodInvocation *context,
                                               const gchar           *action_id,
                                               guint                  num_user_data,
                                               gpointer              *user_data_elements)
{
        const char            *secret  = user_data_elements[0];
        char                 **options = user_data_elements[1];
        devkit_disks_device_luks_unlock_internal (device, secret, options, NULL, NULL, context);
}

gboolean
devkit_disks_device_luks_unlock (DevkitDisksDevice     *device,
                                 const char            *secret,
                                 char                 **options,
                                 DBusGMethodInvocation *context)
{
        if (device->priv->id_usage == NULL ||
            strcmp (device->priv->id_usage, "crypto") != 0) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Not a LUKS device");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              "org.freedesktop.devicekit.disks.luks-unlock",
                                              "LuksUnlock",
                                              TRUE,
                                              devkit_disks_device_luks_unlock_authorized_cb,
                                              context,
                                              2,
                                              g_strdup (secret), g_free,
                                              g_strdupv (options), g_strfreev);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

typedef struct {
        int refcount;
        DBusGMethodInvocation *context;
        DevkitDisksDevice *luks_device;
        DevkitDisksDevice *cleartext_device;
        guint device_removed_signal_handler_id;
        guint device_removed_timeout_id;
} LockEncryptionData;

static LockEncryptionData *
lock_encryption_data_new (DBusGMethodInvocation *context,
                          DevkitDisksDevice *luks_device,
                          DevkitDisksDevice *cleartext_device)
{
        LockEncryptionData *data;

        data = g_new0 (LockEncryptionData, 1);
        data->refcount = 1;

        data->context = context;
        data->luks_device = g_object_ref (luks_device);
        data->cleartext_device = g_object_ref (cleartext_device);
        return data;
}

static LockEncryptionData *
lock_encryption_data_ref (LockEncryptionData *data)
{
        data->refcount++;
        return data;
}

static void
lock_encryption_data_unref (LockEncryptionData *data)
{
        data->refcount--;
        if (data->refcount == 0) {
                g_object_unref (data->luks_device);
                g_object_unref (data->cleartext_device);
                g_free (data);
        }
}


static void
luks_lock_wait_for_cleartext_device_removed_cb (DevkitDisksDaemon *daemon,
                                                     const char *object_path,
                                                     gpointer user_data)
{
        DevkitDisksDevice *device;
        LockEncryptionData *data = user_data;

        device = devkit_disks_daemon_local_find_by_object_path (daemon, object_path);
        if (device == data->cleartext_device) {

                job_local_end (data->luks_device);

                /* update and emit a Changed() signal on the holder since the luks-holder
                 * property indicates the cleartext device
                 */
                update_info (data->luks_device);
                drain_pending_changes (data->luks_device, FALSE);

                dbus_g_method_return (data->context);

                g_signal_handler_disconnect (daemon, data->device_removed_signal_handler_id);
                g_source_remove (data->device_removed_timeout_id);
                lock_encryption_data_unref (data);
        }
}


static gboolean
luks_lock_wait_for_cleartext_device_not_seen_cb (gpointer user_data)
{
        LockEncryptionData *data = user_data;

        job_local_end (data->luks_device);

        throw_error (data->context,
                     DEVKIT_DISKS_ERROR_FAILED,
                     "Error locking luks device: timeout (10s) waiting for cleartext device to be removed");

        g_signal_handler_disconnect (data->cleartext_device->priv->daemon, data->device_removed_signal_handler_id);
        lock_encryption_data_unref (data);
        return FALSE;
}

static void
luks_lock_completed_cb (DBusGMethodInvocation *context,
                             DevkitDisksDevice *device,
                             gboolean job_was_cancelled,
                             int status,
                             const char *stderr,
                             const char *stdout,
                             gpointer user_data)
{
        LockEncryptionData *data = user_data;

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {

                /* if device is already removed, just return */
                if (data->cleartext_device->priv->removed) {
                        /* update and emit a Changed() signal on the holder since the luks-holder
                         * property indicates the cleartext device
                         */
                        update_info (data->luks_device);
                        drain_pending_changes (data->luks_device, FALSE);

                        dbus_g_method_return (context);
                } else {
                        /* otherwise sit and wait for the device to disappear */

                        data->device_removed_signal_handler_id = g_signal_connect_after (
                                device->priv->daemon,
                                "device-removed",
                                (GCallback) luks_lock_wait_for_cleartext_device_removed_cb,
                                lock_encryption_data_ref (data));

                        /* set up timeout for error reporting if waiting failed
                         *
                         * (the signal handler and the timeout handler share the ref to data
                         * as one will cancel the other)
                         */
                        data->device_removed_timeout_id = g_timeout_add (
                                10 * 1000,
                                luks_lock_wait_for_cleartext_device_not_seen_cb,
                                data);

                        job_local_start (device, "LuksLock");
                }
        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error locking device: cryptsetup exited with exit code %d: %s",
                                     WEXITSTATUS (status), stderr);
                }
        }
}

static gboolean
luks_get_uid_from_dm_name (const char *dm_name, uid_t *out_uid)
{
        int n;
        gboolean ret;
        uid_t uid;
        char *endp;

        ret = FALSE;

        if (!g_str_has_prefix (dm_name, "devkit-disks-luks-uuid"))
                goto out;

        /* determine who unlocked the device */
        for (n = strlen (dm_name) - 1; n >= 0; n--) {
                if (dm_name[n] == '-')
                        break;
        }
        if (strncmp (dm_name + n, "-uid", 4) != 0)
                goto out;

        uid = strtol (dm_name + n + 4, &endp, 10);
        if (endp == NULL || *endp != '\0')
                goto out;

        if (out_uid != NULL)
                *out_uid = uid;

        ret = TRUE;
out:
        return ret;
}

static void
devkit_disks_device_luks_lock_authorized_cb (DevkitDisksDaemon     *daemon,
                                             DevkitDisksDevice     *device,
                                             DBusGMethodInvocation *context,
                                             const gchar           *action_id,
                                             guint                  num_user_data,
                                             gpointer              *user_data_elements)
{
        /* TODO: use options */
        //char                 **options = user_data_elements[0];
        DevkitDisksDevice *cleartext_device;
        int n;
        char *argv[10];
        GError *error;

        if (device->priv->id_usage == NULL ||
            strcmp (device->priv->id_usage, "crypto") != 0) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Not a LUKS crypto device");
                goto out;
        }

        cleartext_device = find_cleartext_device (device);
        if (cleartext_device == NULL) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Cleartext device is not unlocked");
                goto out;
        }

        if (cleartext_device->priv->dm_name == NULL || strlen (cleartext_device->priv->dm_name) == 0) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Cannot determine device-mapper name");
                goto out;
        }

        n = 0;
        argv[n++] = "cryptsetup";
        argv[n++] = "luksClose";
        argv[n++] = cleartext_device->priv->dm_name;
        argv[n++] = NULL;

        error = NULL;
        if (!job_new (context,
                      "LuksLock",
                      FALSE,
                      device,
                      argv,
                      NULL,
                      luks_lock_completed_cb,
                      lock_encryption_data_new (context, device, cleartext_device),
                      (GDestroyNotify) lock_encryption_data_unref)) {
                    goto out;
        }

out:
        ;
}

gboolean
devkit_disks_device_luks_lock (DevkitDisksDevice     *device,
                               char                 **options,
                               DBusGMethodInvocation *context)
{
        uid_t unlocked_by_uid;
        uid_t uid;
        DevkitDisksDevice *cleartext_device;
        const gchar *action_id;

        devkit_disks_daemon_local_get_uid (device->priv->daemon, &uid, context);

        if (device->priv->id_usage == NULL ||
            strcmp (device->priv->id_usage, "crypto") != 0) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Not a LUKS crypto device");
                goto out;
        }

        cleartext_device = find_cleartext_device (device);
        if (cleartext_device == NULL) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Cleartext device is not unlocked");
                goto out;
        }

        if (cleartext_device->priv->dm_name == NULL || strlen (cleartext_device->priv->dm_name) == 0) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Cannot determine device-mapper name");
                goto out;
        }

        /* see if we (DeviceKit-disks) set up this clear text device */
        if (!luks_get_uid_from_dm_name (cleartext_device->priv->dm_name, &unlocked_by_uid)) {
                /* nope.. so assume uid 0 set it up.. we still allow locking
                 * the device... given enough privilege
                 */
                unlocked_by_uid = 0;
        }

        /* require authorization if unlocked by someone else */
        action_id = NULL;
        if (unlocked_by_uid != uid) {
                action_id = "org.freedesktop.devicekit.disks.luks-lock-others";
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              action_id,
                                              "LuksLock",
                                              TRUE,
                                              devkit_disks_device_luks_lock_authorized_cb,
                                              context,
                                              1,
                                              g_strdupv (options), g_strfreev);

 out:
        return TRUE;
}


/*--------------------------------------------------------------------------------------------------------------*/

static void
luks_change_passphrase_completed_cb (DBusGMethodInvocation *context,
                                          DevkitDisksDevice *device,
                                          gboolean job_was_cancelled,
                                          int status,
                                          const char *stderr,
                                          const char *stdout,
                                          gpointer user_data)
{
        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {
                dbus_g_method_return (context);
        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error changing secret on device: helper exited with exit code %d: %s",
                                     WEXITSTATUS (status), stderr);
                }
        }
}

static void
devkit_disks_device_luks_change_passphrase_authorized_cb (DevkitDisksDaemon     *daemon,
                                                          DevkitDisksDevice     *device,
                                                          DBusGMethodInvocation *context,
                                                          const gchar           *action_id,
                                                          guint                  num_user_data,
                                                          gpointer              *user_data_elements)
{
        const char            *old_secret = user_data_elements[0];
        const char            *new_secret = user_data_elements[1];
        int n;
        char *argv[10];
        GError *error;
        char *secrets_as_stdin;

        secrets_as_stdin = NULL;

        if (device->priv->id_usage == NULL ||
            strcmp (device->priv->id_usage, "crypto") != 0) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Not a LUKS crypto device");
                goto out;
        }

        secrets_as_stdin = g_strdup_printf ("%s\n%s\n", old_secret, new_secret);

        n = 0;
        argv[n++] = "devkit-disks-helper-change-luks-password";
        argv[n++] = device->priv->device_file;
        argv[n++] = NULL;

        error = NULL;
        if (!job_new (context,
                      "LuksChangePassphrase",
                      FALSE,
                      device,
                      argv,
                      secrets_as_stdin,
                      luks_change_passphrase_completed_cb,
                      NULL,
                      NULL)) {
                goto out;
        }

out:
        /* scrub the secrets */
        if (secrets_as_stdin != NULL) {
                memset (secrets_as_stdin, '\0', strlen (secrets_as_stdin));
        }
        g_free (secrets_as_stdin);
}

gboolean
devkit_disks_device_luks_change_passphrase (DevkitDisksDevice     *device,
                                            const char            *old_secret,
                                            const char            *new_secret,
                                            DBusGMethodInvocation *context)
{
        /* No need to check for busy; we can actually do this while the device is unlocked as
         * only LUKS metadata is modified.
         */

        if (device->priv->id_usage == NULL ||
            strcmp (device->priv->id_usage, "crypto") != 0) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Not a LUKS crypto device");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              device->priv->device_is_system_internal ?
                                                "org.freedesktop.devicekit.disks.change-system-internal" :
                                                "org.freedesktop.devicekit.disks.change",
                                              "LuksChangePassphrase",
                                              TRUE,
                                              devkit_disks_device_luks_change_passphrase_authorized_cb,
                                              context,
                                              2,
                                              g_strdup (old_secret), g_free,
                                              g_strdup (new_secret), g_free);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
filesystem_set_label_completed_cb (DBusGMethodInvocation *context,
                                      DevkitDisksDevice *device,
                                      gboolean job_was_cancelled,
                                      int status,
                                      const char *stderr,
                                      const char *stdout,
                                      gpointer user_data)
{
        char *new_label = user_data;

        /* poke the kernel so we can reread the data */
        devkit_disks_device_generate_kernel_change_event (device);

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {

                /* update local copy, don't wait for the kernel */
                devkit_disks_device_set_id_label (device, new_label);

                drain_pending_changes (device, FALSE);

                dbus_g_method_return (context);

        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error changing fslabel: helper exited with exit code %d: %s",
                                     WEXITSTATUS (status), stderr);
                }
        }
}

static void
devkit_disks_device_filesystem_set_label_authorized_cb (DevkitDisksDaemon     *daemon,
                                                        DevkitDisksDevice     *device,
                                                        DBusGMethodInvocation *context,
                                                        const gchar           *action_id,
                                                        guint                  num_user_data,
                                                        gpointer              *user_data_elements)
{
        const gchar *new_label = user_data_elements[0];
        int n;
        char *argv[10];
        const DevkitDisksFilesystem *fs_details;
        GError *error;

        error = NULL;

        if (device->priv->id_usage == NULL ||
            strcmp (device->priv->id_usage, "filesystem") != 0) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Not a mountable file system");
                goto out;
        }

        fs_details = devkit_disks_daemon_local_get_fs_details (device->priv->daemon,
                                                               device->priv->id_type);
        if (fs_details == NULL) {
                throw_error (context, DEVKIT_DISKS_ERROR_BUSY, "Unknown filesystem");
                goto out;
        }

        if (!fs_details->supports_online_label_rename) {
                if (devkit_disks_device_local_is_busy (device, FALSE, &error)) {
                        dbus_g_method_return_error (context, error);
                        g_error_free (error);
                        goto out;
                }
        }

        n = 0;
        argv[n++] = "devkit-disks-helper-change-filesystem-label";
        argv[n++] = device->priv->device_file;
        argv[n++] = device->priv->id_type;
        argv[n++] = (char *) new_label;
        argv[n++] = NULL;

        if (!job_new (context,
                      "FilesystemSetLabel",
                      FALSE,
                      device,
                      argv,
                      NULL,
                      filesystem_set_label_completed_cb,
                      g_strdup (new_label),
                      g_free)) {
                goto out;
        }

out:
        ;
}

gboolean
devkit_disks_device_filesystem_set_label (DevkitDisksDevice     *device,
                                          const char            *new_label,
                                          DBusGMethodInvocation *context)
{
        const DevkitDisksFilesystem *fs_details;
        GError *error;

        error = NULL;

        if (device->priv->id_usage == NULL ||
            strcmp (device->priv->id_usage, "filesystem") != 0) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Not a mountable file system");
                goto out;
        }

        fs_details = devkit_disks_daemon_local_get_fs_details (device->priv->daemon,
                                                               device->priv->id_type);
        if (fs_details == NULL) {
                throw_error (context, DEVKIT_DISKS_ERROR_BUSY, "Unknown filesystem");
                goto out;
        }

        if (!fs_details->supports_online_label_rename) {
                if (devkit_disks_device_local_is_busy (device, FALSE, &error)) {
                        dbus_g_method_return_error (context, error);
                        g_error_free (error);
                        goto out;
                }
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              device->priv->device_is_system_internal ?
                                                "org.freedesktop.devicekit.disks.change-system-internal" :
                                                "org.freedesktop.devicekit.disks.change",
                                              "FilesystemSetLabel",
                                              TRUE,
                                              devkit_disks_device_filesystem_set_label_authorized_cb,
                                              context,
                                              1,
                                              g_strdup (new_label), g_free);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

/* may be called with context==NULL */
static void
drive_ata_smart_refresh_data_completed_cb (DBusGMethodInvocation *context,
                                           DevkitDisksDevice *device,
                                           gboolean job_was_cancelled,
                                           int status,
                                           const char *stderr,
                                           const char *stdout,
                                           gpointer user_data)
{
        gint rc;
        SkDisk *d;
        gchar *blob;
        gsize blob_size;
        time_t time_collected;
        SkSmartOverall overall;

        d = NULL;
        blob = NULL;

        if (job_was_cancelled || stdout == NULL) {
                if (job_was_cancelled) {
                        if (context != NULL)
                                throw_error (context,
                                             DEVKIT_DISKS_ERROR_CANCELLED,
                                             "Job was cancelled");
                } else {
                        if (context != NULL)
                                throw_error (context,
                                             DEVKIT_DISKS_ERROR_FAILED,
                                             "Error retrieving ATA SMART data: no output",
                                             WEXITSTATUS (status), stderr);
                }
                goto out;
        }

        rc = WEXITSTATUS (status);

        if (rc != 0) {
                if (rc == 2) {
                        if (context != NULL) {
                                throw_error (context,
                                             DEVKIT_DISKS_ERROR_ATA_SMART_WOULD_WAKEUP,
                                             "Error retrieving ATA SMART data: %s",
                                             stderr);
                        }
                } else {
                        if (context != NULL) {
                                throw_error (context,
                                             DEVKIT_DISKS_ERROR_FAILED,
                                             "Error retrieving ATA SMART data: helper failed with exit code %d: %s",
                                             rc, stderr);
                        }
                }
                goto out;
        }

        blob = (gchar *) g_base64_decode (stdout, &blob_size);

        if (sk_disk_open (NULL, &d) != 0) {
                if (context != NULL) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "unable to open a SkDisk");
                }
                goto out;
        }

        if (sk_disk_set_blob (d, blob, blob_size) != 0) {
                if (context != NULL) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "error parsing blob: %s",
                                     strerror (errno));
                }
                goto out;
        }

        time_collected = time (NULL);
        devkit_disks_device_set_drive_ata_smart_time_collected (device, time_collected);

        if (sk_disk_smart_get_overall (d, &overall) != 0)
                overall = -1;
        devkit_disks_device_set_drive_ata_smart_status (device, overall);
        devkit_disks_device_set_drive_ata_smart_blob_steal (device, blob, blob_size);
        blob = NULL;

        /* emit change event since we've updated the smart data */
        drain_pending_changes (device, FALSE);

        if (context != NULL)
                dbus_g_method_return (context);

out:
        g_free (blob);
        if (d != NULL)
                sk_disk_free (d);
}

static void
devkit_disks_device_drive_ata_smart_refresh_data_authorized_cb (DevkitDisksDaemon     *daemon,
                                                                DevkitDisksDevice     *device,
                                                                DBusGMethodInvocation *context,
                                                                const gchar           *action_id,
                                                                guint                  num_user_data,
                                                                gpointer              *user_data_elements)
{
        char                 **options = user_data_elements[0];
        int n;
        char *argv[10];
        GError *error;
        const char *simuldata;
        gboolean nowakeup;
        uid_t caller_uid;

        devkit_disks_daemon_local_get_uid (device->priv->daemon, &caller_uid, context);

        if (!device->priv->drive_ata_smart_is_available) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Device does not support ATA SMART");
                goto out;
        }

        simuldata = NULL;
        nowakeup = FALSE;
        for (n = 0; options[n] != NULL; n++) {
                if (g_str_has_prefix (options[n], "simulate=")) {
                        if (context != NULL) {
                                if (caller_uid != 0) {
                                        throw_error (context,
                                                     DEVKIT_DISKS_ERROR_FAILED,
                                                     "Only uid 0 may use the simulate= option");
                                        goto out;
                                }
                        }
                        simuldata = (const char *) options[n] + 9;
                } else if (strcmp (options[n], "nowakeup") == 0) {
                        nowakeup = TRUE;
                }
        }

        if (simuldata != NULL) {
                n = 0;
                argv[n++] = "base64"; /* provided by coreutils */
                argv[n++] = (char *) simuldata;
                argv[n++] = NULL;
        } else {
                n = 0;
                argv[n++] = PACKAGE_LIBEXEC_DIR "/devkit-disks-helper-ata-smart-collect";
                argv[n++] = device->priv->device_file;
                argv[n++] = nowakeup ? "1" : "0";
                argv[n++] = NULL;
        }

        error = NULL;

        if (!job_new (context,
                      NULL,     /* don't run this as a job */
                      FALSE,
                      device,
                      argv,
                      NULL,
                      drive_ata_smart_refresh_data_completed_cb,
                      NULL,
                      NULL)) {
                goto out;
        }

out:
        ;
}

/* may be called with context==NULL */
gboolean
devkit_disks_device_drive_ata_smart_refresh_data (DevkitDisksDevice     *device,
                                                  char                 **options,
                                                  DBusGMethodInvocation *context)
{
        const gchar *action_id;

        if (!device->priv->drive_ata_smart_is_available) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Device does not support ATA SMART");
                goto out;
        }

        action_id = NULL;
        if (context != NULL) {
                action_id = "org.freedesktop.devicekit.disks.drive-ata-smart-refresh";
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              action_id,
                                              "DriveAtaSmartRefreshData",
                                              TRUE,
                                              devkit_disks_device_drive_ata_smart_refresh_data_authorized_cb,
                                              context,
                                              1,
                                              g_strdupv (options), g_strfreev);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
drive_ata_smart_initiate_selftest_completed_cb (DBusGMethodInvocation *context,
                                                DevkitDisksDevice *device,
                                                gboolean job_was_cancelled,
                                                int status,
                                                const char *stderr,
                                                const char *stdout,
                                                gpointer user_data)
{
        char *options[] = {NULL};

        /* no matter what happened, refresh the data */
        devkit_disks_device_drive_ata_smart_refresh_data (device, options, NULL);

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {

                dbus_g_method_return (context);

        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error running self test: helper exited with exit code %d: %s",
                                     WEXITSTATUS (status), stderr);
                }
        }
}

static void
devkit_disks_device_drive_ata_smart_initiate_selftest_authorized_cb (DevkitDisksDaemon     *daemon,
                                                                     DevkitDisksDevice     *device,
                                                                     DBusGMethodInvocation *context,
                                                                     const gchar           *action_id,
                                                                     guint                  num_user_data,
                                                                     gpointer              *user_data_elements)
{
        const gchar  *test    = user_data_elements[0];
        /* TODO: use options */
        //gchar       **options = user_data_elements[1];
        int n;
        char *argv[10];
        GError *error;
        const gchar *job_name;

        if (g_strcmp0 (test, "short") == 0) {
                job_name = "DriveAtaSmartSelftestShort";
        } else if (g_strcmp0 (test, "extended") == 0) {
                job_name = "DriveAtaSmartSelftestExtended";
        } else if (g_strcmp0 (test, "conveyance") == 0) {
                job_name = "DriveAtaSmartSelftestConveyance";
        } else {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Malformed test");
                goto out;
        }

        n = 0;
        argv[n++] = PACKAGE_LIBEXEC_DIR "/devkit-disks-helper-ata-smart-selftest";
        argv[n++] = device->priv->device_file;
        argv[n++] = (char *) test;
        argv[n++] = NULL;

        error = NULL;
        if (!job_new (context,
                      job_name,
                      TRUE,
                      device,
                      argv,
                      NULL,
                      drive_ata_smart_initiate_selftest_completed_cb,
                      NULL,
                      NULL)) {
                goto out;
        }

out:
        ;
}

gboolean
devkit_disks_device_drive_ata_smart_initiate_selftest (DevkitDisksDevice     *device,
                                                       const char            *test,
                                                       gchar                **options,
                                                       DBusGMethodInvocation *context)
{
        if (!device->priv->drive_ata_smart_is_available) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "Device does not support ATA SMART");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              "org.freedesktop.devicekit.disks.drive-ata-smart-selftest",
                                              "DriveAtaSmartInitiateSelftest",
                                              TRUE,
                                              devkit_disks_device_drive_ata_smart_initiate_selftest_authorized_cb,
                                              context,
                                              2,
                                              g_strdup (test), g_free,
                                              g_strdupv (options), g_strfreev);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
linux_md_stop_completed_cb (DBusGMethodInvocation *context,
                                  DevkitDisksDevice *device,
                                  gboolean job_was_cancelled,
                                  int status,
                                  const char *stderr,
                                  const char *stdout,
                                  gpointer user_data)
{
        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {

                /* the kernel side of md currently doesn't emit a 'changed' event so
                 * generate one such that the md device can disappear from our
                 * database
                 */
                devkit_disks_device_generate_kernel_change_event (device);

                dbus_g_method_return (context);

        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error stopping array: mdadm exited with exit code %d: %s",
                                     WEXITSTATUS (status), stderr);
                }
        }
}

static void
devkit_disks_device_linux_md_stop_authorized_cb (DevkitDisksDaemon     *daemon,
                                                 DevkitDisksDevice     *device,
                                                 DBusGMethodInvocation *context,
                                                 const gchar           *action_id,
                                                 guint                  num_user_data,
                                                 gpointer              *user_data_elements)
{
        /* TODO: use options */
        //gchar       **options = user_data_elements[0];
        int n;
        char *argv[10];
        GError *error;

        n = 0;
        argv[n++] = "mdadm";
        argv[n++] = "--stop";
        argv[n++] = device->priv->device_file;
        argv[n++] = NULL;

        error = NULL;
        if (!job_new (context,
                      "LinuxMdStop",
                      TRUE,
                      device,
                      argv,
                      NULL,
                      linux_md_stop_completed_cb,
                      NULL,
                      NULL)) {
                goto out;
        }

out:
        ;
}

gboolean
devkit_disks_device_linux_md_stop (DevkitDisksDevice     *device,
                                   char                 **options,
                                   DBusGMethodInvocation *context)
{
        if (!device->priv->device_is_linux_md) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a Linux md drive");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              "org.freedesktop.devicekit.disks.linux-md",
                                              "LinuxMdStop",
                                              TRUE,
                                              devkit_disks_device_linux_md_stop_authorized_cb,
                                              context,
                                              1,
                                              g_strdupv (options), g_strfreev);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
linux_md_check_completed_cb (DBusGMethodInvocation *context,
                             DevkitDisksDevice *device,
                             gboolean job_was_cancelled,
                             int status,
                             const char *stderr,
                             const char *stdout,
                             gpointer user_data)
{
        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {
                guint64 num_errors;

                num_errors = sysfs_get_uint64 (device->priv->native_path, "md/mismatch_cnt");

                dbus_g_method_return (context, num_errors);
        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error checking array: helper exited with exit code %d: %s",
                                     WEXITSTATUS (status), stderr);
                }
        }
}

static void
devkit_disks_device_linux_md_check_authorized_cb (DevkitDisksDaemon     *daemon,
                                                  DevkitDisksDevice     *device,
                                                  DBusGMethodInvocation *context,
                                                  const gchar           *action_id,
                                                  guint                  num_user_data,
                                                  gpointer              *user_data_elements)
{
        gchar **options = user_data_elements[0];
        gchar *filename;
        int n, m;
        char *argv[128];
        const gchar *job_name;

        filename = NULL;

        if (!device->priv->device_is_linux_md) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a Linux md drive");
                goto out;
        }

        if (g_strcmp0 (device->priv->linux_md_sync_action, "idle") != 0) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Array is not idle");
                goto out;
        }

        n = 0;
        argv[n++] = PACKAGE_LIBEXEC_DIR "/devkit-disks-helper-linux-md-check";
        argv[n++] = device->priv->device_file;
        argv[n++] = device->priv->native_path;
        for (m = 0; options[m] != NULL; m++) {
                if (n >= (int) sizeof (argv) - 1) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Too many options");
                        goto out;
                }
                /* the helper will validate each option */
                argv[n++] = (char *) options[m];
        }
        argv[n++] = NULL;

        job_name = "LinuxMdCheck";
        for (n = 0; options != NULL && options[n] != NULL; n++)
                if (strcmp (options[n], "repair") == 0)
                        job_name = "LinuxMdRepair";

        if (!job_new (context,
                      job_name,
                      TRUE,
                      device,
                      argv,
                      NULL,
                      linux_md_check_completed_cb,
                      NULL,
                      NULL)) {
                goto out;
        }

 out:
        ;
}

gboolean
devkit_disks_device_linux_md_check (DevkitDisksDevice     *device,
                                    char                 **options,
                                    DBusGMethodInvocation *context)
{
        guint n;
        const gchar *job_name;

        job_name = "LinuxMdCheck";
        for (n = 0; options != NULL && options[n] != NULL; n++)
                if (strcmp (options[n], "repair") == 0)
                        job_name = "LinuxMdRepair";

        if (!device->priv->device_is_linux_md) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a Linux md drive");
                goto out;
        }

        if (g_strcmp0 (device->priv->linux_md_sync_action, "idle") != 0) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Array is not idle");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              "org.freedesktop.devicekit.disks.linux-md",
                                              job_name,
                                              TRUE,
                                              devkit_disks_device_linux_md_check_authorized_cb,
                                              context,
                                              1,
                                              g_strdupv (options), g_strfreev);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
linux_md_add_component_completed_cb (DBusGMethodInvocation *context,
                                              DevkitDisksDevice *device,
                                              gboolean job_was_cancelled,
                                              int status,
                                              const char *stderr,
                                              const char *stdout,
                                              gpointer user_data)
{
        DevkitDisksDevice *slave = DEVKIT_DISKS_DEVICE (user_data);

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {

                /* the slave got new metadata on it; reread that */
                devkit_disks_device_generate_kernel_change_event (slave);

                /* the kernel side of md currently doesn't emit a 'changed' event so
                 * generate one since state may have changed (e.g. rebuild started etc.)
                 */
                devkit_disks_device_generate_kernel_change_event (device);

                dbus_g_method_return (context);

        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error adding component: mdadm exited with exit code %d: %s",
                                     WEXITSTATUS (status), stderr);
                }
        }
}

static void
devkit_disks_device_linux_md_add_component_authorized_cb (DevkitDisksDaemon     *daemon,
                                                          DevkitDisksDevice     *device,
                                                          DBusGMethodInvocation *context,
                                                          const gchar           *action_id,
                                                          guint                  num_user_data,
                                                          gpointer              *user_data_elements)
{
        char                  *component = user_data_elements[0];
        /* TODO: use options */
        //char                 **options   = user_data_elements[1];
        int n;
        char *argv[10];
        GError *error;
        DevkitDisksDevice *slave;

        error = NULL;

        slave = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, component);
        if (slave == NULL) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Component doesn't exist");
                goto out;
        }

        /* it's fine if the given device isn't a Linux md component _yet_; think
         * hot adding a new disk if an old one failed
         */

        if (devkit_disks_device_local_is_busy (slave, TRUE, &error)) {
                dbus_g_method_return_error (context, error);
                g_error_free (error);
                goto out;
        }

        /* TODO: check component size is OK */

        n = 0;
        argv[n++] = "mdadm";
        argv[n++] = "--manage";
        argv[n++] = device->priv->device_file;
        argv[n++] = "--add";
        argv[n++] = slave->priv->device_file;
        argv[n++] = "--force";
        argv[n++] = NULL;

        if (!job_new (context,
                      "LinuxMdAddComponent",
                      TRUE,
                      device,
                      argv,
                      NULL,
                      linux_md_add_component_completed_cb,
                      g_object_ref (slave),
                      g_object_unref)) {
                goto out;
        }

out:
        ;
}

gboolean
devkit_disks_device_linux_md_add_component (DevkitDisksDevice     *device,
                                            char                  *component,
                                            char                 **options,
                                            DBusGMethodInvocation *context)
{
        if (!device->priv->device_is_linux_md) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a Linux md drive");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              "org.freedesktop.devicekit.disks.linux-md",
                                              "LinuxMdAddComponent",
                                              TRUE,
                                              devkit_disks_device_linux_md_add_component_authorized_cb,
                                              context,
                                              2,
                                              g_strdup (component), g_free,
                                              g_strdupv (options), g_strfreev);
 out:
        return TRUE;
}


/*--------------------------------------------------------------------------------------------------------------*/


typedef struct {
        int refcount;

        DBusGMethodInvocation *context;
        DevkitDisksDevice *slave;
        char **options;

        guint device_changed_signal_handler_id;
        guint device_changed_timeout_id;

} RemoveComponentData;

static RemoveComponentData *
remove_component_data_new (DBusGMethodInvocation      *context,
                           DevkitDisksDevice          *slave,
                           char                      **options)
{
        RemoveComponentData *data;

        data = g_new0 (RemoveComponentData, 1);
        data->refcount = 1;

        data->context = context;
        data->slave = g_object_ref (slave);
        data->options = g_strdupv (options);
        return data;
}

static RemoveComponentData *
remove_component_data_ref (RemoveComponentData *data)
{
        data->refcount++;
        return data;
}

static void
remove_component_data_unref (RemoveComponentData *data)
{
        data->refcount--;
        if (data->refcount == 0) {
                g_object_unref (data->slave);
                g_free (data->options);
                g_free (data);
        }
}


static void
linux_md_remove_component_device_changed_cb (DevkitDisksDaemon *daemon,
                                             const char *object_path,
                                             gpointer user_data)
{
        RemoveComponentData *data = user_data;
        DevkitDisksDevice *device;
        GError *error;

        error = NULL;

        device = devkit_disks_daemon_local_find_by_object_path (daemon, object_path);
        if (device == data->slave) {

                if (devkit_disks_device_local_is_busy (data->slave, FALSE, &error)) {
                        dbus_g_method_return_error (data->context, error);
                        g_error_free (error);
                } else {
                        gchar *fs_create_options[] = {NULL};

                        /* yay! now scrub it! */
                        devkit_disks_device_filesystem_create (data->slave,
                                                               "empty",
                                                               fs_create_options,
                                                               data->context);

                        /* TODO: leaking data? */

                        g_signal_handler_disconnect (daemon, data->device_changed_signal_handler_id);
                        g_source_remove (data->device_changed_timeout_id);
                }
        }
}

static gboolean
linux_md_remove_component_device_not_seen_cb (gpointer user_data)
{
        RemoveComponentData *data = user_data;

        throw_error (data->context,
                     DEVKIT_DISKS_ERROR_FAILED,
                     "Error removing component: timeout (10s) waiting for slave to stop being busy");

        g_signal_handler_disconnect (data->slave->priv->daemon, data->device_changed_signal_handler_id);
        remove_component_data_unref (data);

        return FALSE;
}


static void
linux_md_remove_component_completed_cb (DBusGMethodInvocation *context,
                                                   DevkitDisksDevice *device,
                                                   gboolean job_was_cancelled,
                                                   int status,
                                                   const char *stderr,
                                                   const char *stdout,
                                                   gpointer user_data)
{
        RemoveComponentData *data = user_data;

        /* the slave got new metadata on it; reread that */
        devkit_disks_device_generate_kernel_change_event (data->slave);

        /* the kernel side of md currently doesn't emit a 'changed' event so
         * generate one since state may have changed (e.g. rebuild started etc.)
         */
        devkit_disks_device_generate_kernel_change_event (device);

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {

                /* wait for the slave to be busy, then start erasing it */

                data->device_changed_signal_handler_id = g_signal_connect_after (
                        device->priv->daemon,
                        "device-changed",
                        (GCallback) linux_md_remove_component_device_changed_cb,
                        remove_component_data_ref (data));

                /* set up timeout for error reporting if waiting failed
                 *
                 * (the signal handler and the timeout handler share the ref to data
                 * as one will cancel the other)
                 */
                data->device_changed_timeout_id = g_timeout_add (
                        10 * 1000,
                        linux_md_remove_component_device_not_seen_cb,
                        data);

        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error removing component: helper exited with exit code %d: %s",
                                     WEXITSTATUS (status), stderr);
                }
        }
}

static void
devkit_disks_device_linux_md_remove_component_authorized_cb (DevkitDisksDaemon     *daemon,
                                                             DevkitDisksDevice     *device,
                                                             DBusGMethodInvocation *context,
                                                             const gchar           *action_id,
                                                             guint                  num_user_data,
                                                             gpointer              *user_data_elements)
{
        char                  *component = user_data_elements[0];
        char                 **options   = user_data_elements[1];
        int n, m;
        char *argv[128];
        GError *error;
        DevkitDisksDevice *slave;

        slave = devkit_disks_daemon_local_find_by_object_path (device->priv->daemon, component);
        if (slave == NULL) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Component doesn't exist");
                goto out;
        }

        /* check that it really is a component of the md device */
        for (n = 0; n < (int) device->priv->linux_md_slaves->len; n++) {
                if (strcmp (component, device->priv->linux_md_slaves->pdata[n]) == 0)
                        break;
        }
        if (n == (int) device->priv->linux_md_slaves->len) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Component isn't part of the running array");
                goto out;
        }

        n = 0;
        argv[n++] = PACKAGE_LIBEXEC_DIR "/devkit-disks-helper-linux-md-remove-component";
        argv[n++] = device->priv->device_file;
        argv[n++] = slave->priv->device_file;
        for (m = 0; options[m] != NULL; m++) {
                if (n >= (int) sizeof (argv) - 1) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Too many options");
                        goto out;
                }
                /* the helper will validate each option */
                argv[n++] = (char *) options[m];
        }
        argv[n++] = NULL;

        error = NULL;
        if (!job_new (context,
                      "LinuxMdRemoveComponent",
                      TRUE,
                      device,
                      argv,
                      NULL,
                      linux_md_remove_component_completed_cb,
                      remove_component_data_new (context, slave, options),
                      (GDestroyNotify) remove_component_data_unref)) {
                goto out;
        }

out:
        ;
}

gboolean
devkit_disks_device_linux_md_remove_component (DevkitDisksDevice     *device,
                                               char                  *component,
                                               char                 **options,
                                               DBusGMethodInvocation *context)
{
        if (!device->priv->device_is_linux_md) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a Linux md drive");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              "org.freedesktop.devicekit.disks.linux-md",
                                              "LinuxMdRemoveComponent",
                                              TRUE,
                                              devkit_disks_device_linux_md_remove_component_authorized_cb,
                                              context,
                                              2,
                                              g_strdup (component), g_free,
                                              g_strdupv (options), g_strfreev);
 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

typedef struct {
        int refcount;

        guint device_added_signal_handler_id;
        guint device_added_timeout_id;

        DBusGMethodInvocation *context;

        DevkitDisksDaemon *daemon;
        char *uuid;

} LinuxMdStartData;

static LinuxMdStartData *
linux_md_start_data_new (DBusGMethodInvocation *context,
                         DevkitDisksDaemon     *daemon,
                         const char            *uuid)
{
        LinuxMdStartData *data;

        data = g_new0 (LinuxMdStartData, 1);
        data->refcount = 1;

        data->context = context;
        data->daemon = g_object_ref (daemon);
        data->uuid = g_strdup (uuid);
        return data;
}

static LinuxMdStartData *
linux_md_start_data_ref (LinuxMdStartData *data)
{
        data->refcount++;
        return data;
}

static void
linux_md_start_data_unref (LinuxMdStartData *data)
{
        data->refcount--;
        if (data->refcount == 0) {
                g_object_unref (data->daemon);
                g_free (data->uuid);
                g_free (data);
        }
}

static void
linux_md_start_device_added_cb (DevkitDisksDaemon *daemon,
                                const char *object_path,
                                gpointer user_data)
{
        LinuxMdStartData *data = user_data;
        DevkitDisksDevice *device;

        /* check the device is the one we're looking for */
        device = devkit_disks_daemon_local_find_by_object_path (daemon, object_path);

        if (device != NULL &&
            device->priv->device_is_linux_md) {

                /* TODO: actually check this properly by looking at slaves vs. components */

                /* yay! it is.. return value to the user */
                dbus_g_method_return (data->context, object_path);

                g_signal_handler_disconnect (daemon, data->device_added_signal_handler_id);
                g_source_remove (data->device_added_timeout_id);
                linux_md_start_data_unref (data);
        }
}

static gboolean
linux_md_start_device_not_seen_cb (gpointer user_data)
{
        LinuxMdStartData *data = user_data;

        throw_error (data->context,
                     DEVKIT_DISKS_ERROR_FAILED,
                     "Error assembling array: timeout (10s) waiting for array to show up");

        g_signal_handler_disconnect (data->daemon, data->device_added_signal_handler_id);
        linux_md_start_data_unref (data);
        return FALSE;
}

/* NOTE: This is job completion callback from a method on the daemon, not the device. */

static void
linux_md_start_completed_cb (DBusGMethodInvocation *context,
                             DevkitDisksDevice *device,
                             gboolean job_was_cancelled,
                             int status,
                             const char *stderr,
                             const char *stdout,
                             gpointer user_data)
{
        LinuxMdStartData *data = user_data;

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {
                GList *l;
                GList *devices;
                char *objpath;

                /* see if the component appeared already */

                objpath = NULL;

                devices = devkit_disks_daemon_local_get_all_devices (data->daemon);
                for (l = devices; l != NULL; l = l->next) {
                        DevkitDisksDevice *device = DEVKIT_DISKS_DEVICE (l->data);

                        if (device->priv->device_is_linux_md) {

                                /* TODO: check properly */

                                /* yup, return to caller */
                                objpath = device->priv->object_path;
                                break;
                        }
                }

                g_list_free (devices);

                if (objpath != NULL) {
                        dbus_g_method_return (context, objpath);
                } else {
                        /* sit around and wait for the md array to appear */

                        /* sit around wait for the cleartext device to appear */
                        data->device_added_signal_handler_id = g_signal_connect_after (
                                data->daemon,
                                "device-added",
                                (GCallback) linux_md_start_device_added_cb,
                                linux_md_start_data_ref (data));

                        /* set up timeout for error reporting if waiting failed
                         *
                         * (the signal handler and the timeout handler share the ref to data
                         * as one will cancel the other)
                         */
                        data->device_added_timeout_id = g_timeout_add (10 * 1000,
                                                                       linux_md_start_device_not_seen_cb,
                                                                       data);
                }


        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error assembling array: mdadm exited with exit code %d: %s",
                                     WEXITSTATUS (status), stderr);
                }
        }
}

/* NOTE: This is a method on the daemon, not the device. */
static void
devkit_disks_daemon_linux_md_start_authorized_cb (DevkitDisksDaemon     *daemon,
                                                  DevkitDisksDevice     *device,
                                                  DBusGMethodInvocation *context,
                                                  const gchar           *action_id,
                                                  guint                  num_user_data,
                                                  gpointer              *user_data_elements)
{
        gchar **components_as_strv = user_data_elements[0];
        /* TODO: use options */
        //gchar **options            = user_data_elements[1];
        int n;
        int m;
        char *argv[128];
        GError *error;
        char *uuid;
        char *md_device_file;

        uuid = NULL;
        md_device_file = NULL;
        error = NULL;

        /* check that all given components exist, that they are indeed linux-md-components and
         * that their uuid agrees
         */
        for (n = 0; components_as_strv[n] != NULL; n++) {
                DevkitDisksDevice *slave;
                const char *component_objpath = components_as_strv[n];

                slave = devkit_disks_daemon_local_find_by_object_path (daemon, component_objpath);
                if (slave == NULL) {
                        throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                                     "Component %s doesn't exist", component_objpath);
                        goto out;
                }

                if (!slave->priv->device_is_linux_md_component) {
                        throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                                     "%s is not a linux-md component", component_objpath);
                        goto out;
                }

                if (n == 0) {
                        uuid = g_strdup (slave->priv->linux_md_component_uuid);
                        if (uuid == NULL) {
                                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                                             "no uuid for one of the components");
                                goto out;
                        }
                } else {
                        const char *this_uuid;
                        this_uuid = slave->priv->linux_md_component_uuid;

                        if (this_uuid == NULL || strcmp (uuid, this_uuid) != 0) {
                                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                                             "uuid mismatch between given components");
                                goto out;
                        }
                }

                if (devkit_disks_device_local_is_busy (slave, FALSE, &error)) {
                        dbus_g_method_return_error (context, error);
                        g_error_free (error);
                        goto out;
                }
        }

        /* find an unused md minor... Man, I wish mdadm could do this itself; this is slightly racy */
        for (n = 0; TRUE; n++) {
                char *native_path;
                char *array_state;

                /* TODO: move to /sys/class/block instead */
                native_path = g_strdup_printf ("/sys/block/md%d", n);
                if (!sysfs_file_exists (native_path, "md/array_state")) {
                        /* Apparently this slot is free since there is no such file. So let's peruse it. */
                        g_free (native_path);
                        break;
                } else {
                        array_state = sysfs_get_string (native_path, "md/array_state");
                        g_strstrip (array_state);
                        if (strcmp (array_state, "clear") == 0) {
                                /* It's clear! Let's use it! */
                                g_free (array_state);
                                g_free (native_path);
                                break;
                        }
                        g_free (array_state);
                }
                g_free (native_path);
        }

        md_device_file = g_strdup_printf ("/dev/md%d", n);

        n = 0;
        argv[n++] = "mdadm";
        argv[n++] = "--assemble";
        argv[n++] = md_device_file;
        argv[n++] = "--run";
        for (m = 0; components_as_strv[m] != NULL; m++) {
                DevkitDisksDevice *slave;
                const char *component_objpath = components_as_strv[m];

                slave = devkit_disks_daemon_local_find_by_object_path (daemon, component_objpath);
                if (slave == NULL) {
                        throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                                     "Component %s doesn't exist", component_objpath);
                        goto out;
                }

                if (n >= (int) sizeof (argv) - 1) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Too many components");
                        goto out;
                }

                argv[n++] = (char *) slave->priv->device_file;
        }
        argv[n++] = NULL;

        if (!job_new (context,
                      "LinuxMdStart",
                      TRUE,
                      NULL,
                      argv,
                      NULL,
                      linux_md_start_completed_cb,
                      linux_md_start_data_new (context, daemon, uuid),
                      (GDestroyNotify) linux_md_start_data_unref)) {
                goto out;
        }

out:
        g_free (uuid);
        g_free (md_device_file);
}

/* NOTE: This is a method on the daemon, not the device. */
gboolean
devkit_disks_daemon_linux_md_start (DevkitDisksDaemon     *daemon,
                                    GPtrArray             *components,
                                    char                 **options,
                                    DBusGMethodInvocation *context)
{
        gchar **components_as_strv;
        guint n;

        components_as_strv = g_new0 (gchar *, components->len + 1);
        for (n = 0; n < components->len; n++)
                components_as_strv[n] = g_strdup (components->pdata[n]);

        devkit_disks_daemon_local_check_auth (daemon,
                                              NULL,
                                              "org.freedesktop.devicekit.disks.linux-md",
                                              "LinuxMdStart",
                                              TRUE,
                                              devkit_disks_daemon_linux_md_start_authorized_cb,
                                              context,
                                              2,
                                              components_as_strv, g_strfreev,
                                              g_strdupv (options), g_strfreev);

        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

typedef struct {
        int refcount;

        guint device_added_signal_handler_id;
        guint device_added_timeout_id;

        DBusGMethodInvocation *context;

        DevkitDisksDaemon *daemon;
        char *first_component_objpath;

} LinuxMdCreateData;

static LinuxMdCreateData *
linux_md_create_data_new (DBusGMethodInvocation *context,
                          DevkitDisksDaemon     *daemon,
                          const char            *first_component_objpath)
{
        LinuxMdCreateData *data;

        data = g_new0 (LinuxMdCreateData, 1);
        data->refcount = 1;

        data->context = context;
        data->daemon = g_object_ref (daemon);
        data->first_component_objpath = g_strdup (first_component_objpath);
        return data;
}

static LinuxMdCreateData *
linux_md_create_data_ref (LinuxMdCreateData *data)
{
        data->refcount++;
        return data;
}

static void
linux_md_create_data_unref (LinuxMdCreateData *data)
{
        data->refcount--;
        if (data->refcount == 0) {
                g_object_unref (data->daemon);
                g_free (data->first_component_objpath);
                g_free (data);
        }
}

static void
linux_md_create_device_added_cb (DevkitDisksDaemon *daemon,
                                const char *object_path,
                                gpointer user_data)
{
        LinuxMdCreateData *data = user_data;
        DevkitDisksDevice *device;

        /* check the device is the one we're looking for */
        device = devkit_disks_daemon_local_find_by_object_path (daemon, object_path);

        if (device != NULL &&
            device->priv->device_is_linux_md) {

                /* TODO: actually check this properly by looking at slaves vs. components */

                /* yay! it is.. return value to the user */
                dbus_g_method_return (data->context, object_path);

                g_signal_handler_disconnect (daemon, data->device_added_signal_handler_id);
                g_source_remove (data->device_added_timeout_id);
                linux_md_create_data_unref (data);
        }
}

static gboolean
linux_md_create_device_not_seen_cb (gpointer user_data)
{
        LinuxMdCreateData *data = user_data;

        throw_error (data->context,
                     DEVKIT_DISKS_ERROR_FAILED,
                     "Error assembling array: timeout (10s) waiting for array to show up");

        g_signal_handler_disconnect (data->daemon, data->device_added_signal_handler_id);
        linux_md_create_data_unref (data);
        return FALSE;
}

/* NOTE: This is job completion callback from a method on the daemon, not the device. */

static void
linux_md_create_completed_cb (DBusGMethodInvocation *context,
                             DevkitDisksDevice *device,
                             gboolean job_was_cancelled,
                             int status,
                             const char *stderr,
                             const char *stdout,
                             gpointer user_data)
{
        LinuxMdCreateData *data = user_data;

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {
                GList *l;
                GList *devices;
                char *objpath;

                /* see if the component appeared already */

                objpath = NULL;

                devices = devkit_disks_daemon_local_get_all_devices (data->daemon);
                for (l = devices; l != NULL; l = l->next) {
                        DevkitDisksDevice *device = DEVKIT_DISKS_DEVICE (l->data);

                        if (device->priv->device_is_linux_md) {

                                /* TODO: check properly */

                                /* yup, return to caller */
                                objpath = device->priv->object_path;
                                break;
                        }
                }

                g_list_free (devices);

                if (objpath != NULL) {
                        dbus_g_method_return (context, objpath);
                } else {
                        /* sit around and wait for the md array to appear */

                        /* sit around wait for the cleartext device to appear */
                        data->device_added_signal_handler_id = g_signal_connect_after (
                                data->daemon,
                                "device-added",
                                (GCallback) linux_md_create_device_added_cb,
                                linux_md_create_data_ref (data));

                        /* set up timeout for error reporting if waiting failed
                         *
                         * (the signal handler and the timeout handler share the ref to data
                         * as one will cancel the other)
                         */
                        data->device_added_timeout_id = g_timeout_add (10 * 1000,
                                                                       linux_md_create_device_not_seen_cb,
                                                                       data);
                }


        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error assembling array: mdadm exited with exit code %d: %s",
                                     WEXITSTATUS (status), stderr);
                }
        }
}

/* NOTE: This is a method on the daemon, not the device. */
static void
devkit_disks_daemon_linux_md_create_authorized_cb (DevkitDisksDaemon     *daemon,
                                                   DevkitDisksDevice     *device,
                                                   DBusGMethodInvocation *context,
                                                   const gchar           *action_id,
                                                   guint                  num_user_data,
                                                   gpointer              *user_data_elements)
{
        gchar **components_as_strv = user_data_elements[0];
        gchar *level = user_data_elements[1];
        guint64 stripe_size = *((guint64*) user_data_elements[2]);
        gchar *name = user_data_elements[3];
        /* TODO: use options */
        //gchar **options            = user_data_elements[4];
        int n;
        int m;
        char *argv[128];
        GError *error;
        gchar *md_device_file;
        gchar *num_raid_devices_as_str;
        gchar *stripe_size_as_str;
        gboolean use_bitmap;
        gboolean use_chunk;

        md_device_file = NULL;
        num_raid_devices_as_str = NULL;
        stripe_size_as_str = NULL;
        error = NULL;

        /* sanity-check level */
        use_bitmap = FALSE;
        use_chunk = FALSE;
        if (g_strcmp0 (level, "raid0") == 0) {
                use_chunk = TRUE;
        } else if (g_strcmp0 (level, "raid1") == 0) {
                if (stripe_size > 0) {
                        throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                                     "Stripe size doesn't make sense for RAID-1");
                        goto out;
                }
        } else if (g_strcmp0 (level, "raid4") == 0 ||
                   g_strcmp0 (level, "raid5") == 0 ||
                   g_strcmp0 (level, "raid6") == 0 ||
                   g_strcmp0 (level, "raid10") == 0) {
                use_bitmap = TRUE;
                use_chunk = TRUE;
        } else {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Invalid level `%s'",
                             level);
                goto out;
        }

        /* check that all given components exist and that they are not busy
         */
        for (n = 0; components_as_strv[n] != NULL; n++) {
                DevkitDisksDevice *slave;
                const char *component_objpath = components_as_strv[n];

                slave = devkit_disks_daemon_local_find_by_object_path (daemon, component_objpath);
                if (slave == NULL) {
                        throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                                     "Component %s doesn't exist", component_objpath);
                        goto out;
                }

                if (devkit_disks_device_local_is_busy (slave, FALSE, &error)) {
                        dbus_g_method_return_error (context, error);
                        g_error_free (error);
                        goto out;
                }
        }

        /* find an unused md minor... Man, I wish mdadm could do this itself; this is slightly racy */
        for (n = 0; TRUE; n++) {
                char *native_path;
                char *array_state;

                /* TODO: move to /sys/class/block instead */
                native_path = g_strdup_printf ("/sys/block/md%d", n);
                if (!sysfs_file_exists (native_path, "md/array_state")) {
                        /* Apparently this slot is free since there is no such file. So let's peruse it. */
                        g_free (native_path);
                        break;
                } else {
                        array_state = sysfs_get_string (native_path, "md/array_state");
                        g_strstrip (array_state);
                        if (strcmp (array_state, "clear") == 0) {
                                /* It's clear! Let's use it! */
                                g_free (array_state);
                                g_free (native_path);
                                break;
                        }
                        g_free (array_state);
                }
                g_free (native_path);
        }

        md_device_file = g_strdup_printf ("/dev/md%d", n);

        num_raid_devices_as_str = g_strdup_printf ("%d", g_strv_length (components_as_strv));

        if (stripe_size > 0)
                stripe_size_as_str = g_strdup_printf ("%d", ((gint) stripe_size) / 1024);

        n = 0;
        argv[n++] = "mdadm";
        argv[n++] = "--create";
        argv[n++] = md_device_file;
        argv[n++] = "--level";
        argv[n++] = level;
        argv[n++] = "--raid-devices";
        argv[n++] = num_raid_devices_as_str;
        argv[n++] = "--metadata";
        argv[n++] = "1.2";
        argv[n++] = "--name";
        argv[n++] = name;
        argv[n++] = "--homehost";
        argv[n++] = "";
        if (use_bitmap) {
                argv[n++] = "--bitmap";
                argv[n++] = "internal";
        }
        if (use_chunk && stripe_size_as_str != NULL) {
                argv[n++] = "--chunk";
                argv[n++] = stripe_size_as_str;
        }
        for (m = 0; components_as_strv[m] != NULL; m++) {
                DevkitDisksDevice *slave;
                const char *component_objpath = components_as_strv[m];

                slave = devkit_disks_daemon_local_find_by_object_path (daemon, component_objpath);
                if (slave == NULL) {
                        throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                                     "Component %s doesn't exist", component_objpath);
                        goto out;
                }

                if (n >= (int) sizeof (argv) - 1) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Too many components");
                        goto out;
                }

                argv[n++] = (char *) slave->priv->device_file;
        }
        argv[n++] = NULL;

        for (m = 0; argv[m] != NULL; m++)
                g_debug ("arg[%d] = `%s'", m, argv[m]);

        if (!job_new (context,
                      "LinuxMdCreate",
                      TRUE,
                      NULL,
                      argv,
                      NULL,
                      linux_md_create_completed_cb,
                      linux_md_create_data_new (context, daemon, components_as_strv[0]),
                      (GDestroyNotify) linux_md_create_data_unref)) {
                goto out;
        }

out:
        g_free (md_device_file);
        g_free (num_raid_devices_as_str);
        g_free (stripe_size_as_str);
}

/* NOTE: This is a method on the daemon, not the device. */
gboolean
devkit_disks_daemon_linux_md_create (DevkitDisksDaemon     *daemon,
                                     GPtrArray             *components,
                                     char                  *level,
                                     guint64                stripe_size,
                                     char                  *name,
                                     char                 **options,
                                     DBusGMethodInvocation *context)
{
        gchar **components_as_strv;
        guint n;

        components_as_strv = g_new0 (gchar *, components->len + 1);
        for (n = 0; n < components->len; n++)
                components_as_strv[n] = g_strdup (components->pdata[n]);

        devkit_disks_daemon_local_check_auth (daemon,
                                              NULL,
                                              "org.freedesktop.devicekit.disks.linux-md",
                                              "LinuxMdCreate",
                                              TRUE,
                                              devkit_disks_daemon_linux_md_create_authorized_cb,
                                              context,
                                              4,
                                              components_as_strv, g_strfreev,
                                              g_strdup (level), g_free,
                                              g_memdup (&stripe_size, sizeof (guint64)), g_free,
                                              g_strdup (name), g_free,
                                              g_strdupv (options), g_strfreev);

        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

typedef struct {
        char                     *mount_path;
        ForceRemovalCompleteFunc  fr_callback;
        gpointer                  fr_user_data;
} ForceUnmountData;

static ForceUnmountData *
force_unmount_data_new (const gchar              *mount_path,
                        ForceRemovalCompleteFunc  fr_callback,
                        gpointer                  fr_user_data)
{
        ForceUnmountData *data;

        data = g_new0 (ForceUnmountData, 1);
        data->mount_path = g_strdup (mount_path);
        data->fr_callback = fr_callback;
        data->fr_user_data = fr_user_data;

        return data;
}

static void
force_unmount_data_unref (ForceUnmountData *data)
{
        g_free (data->mount_path);
        g_free (data);
}

static void
force_unmount_completed_cb (DBusGMethodInvocation *context,
                            DevkitDisksDevice *device,
                            gboolean job_was_cancelled,
                            int status,
                            const char *stderr,
                            const char *stdout,
                            gpointer user_data)
{
        ForceUnmountData *data = user_data;

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {

                g_print ("**** NOTE: Successfully force unmounted device %s\n", device->priv->device_file);
                /* update_info_mount_state() will update the mounts file and clean up the directory if needed */
                update_info (device);

                if (data->fr_callback != NULL)
                        data->fr_callback (device, TRUE, data->fr_user_data);
        } else {
                g_print ("**** NOTE: force unmount failed: %s\n", stderr);
                if (data->fr_callback != NULL)
                        data->fr_callback (device, FALSE, data->fr_user_data);
        }
}

static void
force_unmount (DevkitDisksDevice        *device,
               ForceRemovalCompleteFunc  callback,
               gpointer                  user_data)
{
        int n;
        char *argv[16];
        const gchar *mount_path;

        mount_path = ((gchar **) device->priv->device_mount_paths->pdata)[0];

        n = 0;
        argv[n++] = "umount";
        /* on Linux, we only have lazy unmount for now */
        argv[n++] = "-l";
        argv[n++] = (gchar *) mount_path;
        argv[n++] = NULL;

        if (!job_new (NULL,
                      "ForceUnmount",
                      FALSE,
                      device,
                      argv,
                      NULL,
                      force_unmount_completed_cb,
                      force_unmount_data_new (mount_path, callback, user_data),
                      (GDestroyNotify) force_unmount_data_unref)) {
                g_warning ("Couldn't spawn unmount for force unmounting %s", mount_path);
                if (callback != NULL)
                        callback (device, FALSE, user_data);
        }
}

/*--------------------------------------------------------------------------------------------------------------*/

typedef struct {
        DevkitDisksDevice        *device;
        char                     *dm_name;
        ForceRemovalCompleteFunc  fr_callback;
        gpointer                  fr_user_data;
} ForceLuksTeardownData;

static void
force_luks_teardown_completed_cb (DBusGMethodInvocation *context,
                                    DevkitDisksDevice *device,
                                    gboolean job_was_cancelled,
                                    int status,
                                    const char *stderr,
                                    const char *stdout,
                                    gpointer user_data)
{
        ForceLuksTeardownData *data = user_data;

        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {

                g_print ("**** NOTE: Successfully teared down luks device %s\n", device->priv->device_file);

                if (data->fr_callback != NULL)
                        data->fr_callback (device, TRUE, data->fr_user_data);
        } else {
                g_print ("**** NOTE: force luks teardown failed: %s\n", stderr);
                if (data->fr_callback != NULL)
                        data->fr_callback (device, FALSE, data->fr_user_data);
        }
}

static ForceLuksTeardownData *
force_luks_teardown_data_new (DevkitDisksDevice        *device,
                                const char               *dm_name,
                                ForceRemovalCompleteFunc  fr_callback,
                                gpointer                  fr_user_data)
{
        ForceLuksTeardownData *data;

        data = g_new0 (ForceLuksTeardownData, 1);
        data->device = g_object_ref (device);
        data->dm_name = g_strdup (dm_name);
        data->fr_callback = fr_callback;
        data->fr_user_data = fr_user_data;
        return data;
}

static void
force_luks_teardown_data_unref (ForceLuksTeardownData *data)
{
        if (data->device != NULL)
                g_object_unref (data->device);
        g_free (data->dm_name);
        g_free (data);
}

static void
force_luks_teardown_cleartext_done (DevkitDisksDevice *device,
                                      gboolean success,
                                      gpointer user_data)
{
        int n;
        char *argv[16];
        ForceLuksTeardownData *data = user_data;

        if (!success) {
                if (data->fr_callback != NULL)
                        data->fr_callback (data->device, FALSE, data->fr_user_data);

                force_luks_teardown_data_unref (data);
                goto out;
        }

        /* ok, clear text device is out of the way; now tear it down */

        n = 0;
        argv[n++] = "cryptsetup";
        argv[n++] = "luksClose";
        argv[n++] = data->dm_name;
        argv[n++] = NULL;

        //g_debug ("doing cryptsetup luksClose %s", data->dm_name);

        if (!job_new (NULL,
                      "ForceLuksTeardown",
                      FALSE,
                      data->device,
                      argv,
                      NULL,
                      force_luks_teardown_completed_cb,
                      data,
                      (GDestroyNotify) force_luks_teardown_data_unref)) {

                g_warning ("Couldn't spawn cryptsetup for force teardown for device %s", data->dm_name);
                if (data->fr_callback != NULL)
                        data->fr_callback (data->device, FALSE, data->fr_user_data);

                force_luks_teardown_data_unref (data);
        }
out:
        ;
}

static void
force_luks_teardown (DevkitDisksDevice        *device,
                       DevkitDisksDevice        *cleartext_device,
                       ForceRemovalCompleteFunc  callback,
                       gpointer                  user_data)
{
        /* first we gotta force remove the clear text device */
        force_removal (cleartext_device,
                       force_luks_teardown_cleartext_done,
                       force_luks_teardown_data_new (device,
                                                       cleartext_device->priv->dm_name,
                                                       callback,
                                                       user_data));
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
force_removal (DevkitDisksDevice        *device,
               ForceRemovalCompleteFunc  callback,
               gpointer                  user_data)
{
        //g_debug ("in force removal for %s", device->priv->device_file);

        /* Device is going bye bye. If this device is
         *
         *  - Mounted by us, then forcibly unmount it.
         *
         *  - If it's a luks device, check if there's cleartext
         *    companion. If so, tear it down if it was setup by us.
         *
         */
        if (device->priv->device_is_mounted && device->priv->device_mount_paths->len > 0) {
                gboolean remove_dir_on_unmount;

                if (devkit_disks_mount_file_has_device (device->priv->device_file, NULL, &remove_dir_on_unmount)) {
                        g_print ("**** NOTE: Force unmounting device %s\n", device->priv->device_file);
                        force_unmount (device, callback, user_data);
                        goto pending;
                }
        }

        if (device->priv->id_usage != NULL && strcmp (device->priv->id_usage, "crypto") == 0) {
                GList *devices;
                GList *l;

                /* look for cleartext device  */
                devices = devkit_disks_daemon_local_get_all_devices (device->priv->daemon);
                for (l = devices; l != NULL; l = l->next) {
                        DevkitDisksDevice *d = DEVKIT_DISKS_DEVICE (l->data);
                        if (d->priv->device_is_luks_cleartext &&
                            d->priv->luks_cleartext_slave != NULL &&
                            strcmp (d->priv->luks_cleartext_slave, device->priv->object_path) == 0) {

                                /* Check whether it is set up by us */
                                if (d->priv->dm_name != NULL &&
                                    g_str_has_prefix (d->priv->dm_name, "devkit-disks-luks-uuid-")) {

                                        g_print ("**** NOTE: Force luks teardown device %s (cleartext %s)\n",
                                                 device->priv->device_file,
                                                 d->priv->device_file);

                                        /* Gotcha */
                                        force_luks_teardown (device, d, callback, user_data);
                                        goto pending;
                                }
                        }
                }
        }

        /* nothing to force remove */
        if (callback != NULL)
                callback (device, TRUE, user_data);

pending:
        ;
}


/*--------------------------------------------------------------------------------------------------------------*/

static void
polling_inhibitor_disconnected_cb (DevkitDisksInhibitor *inhibitor,
                                   DevkitDisksDevice   *device)
{
        device->priv->polling_inhibitors = g_list_remove (device->priv->polling_inhibitors, inhibitor);
        g_signal_handlers_disconnect_by_func (inhibitor, polling_inhibitor_disconnected_cb, device);
        g_object_unref (inhibitor);

        update_info (device);
        drain_pending_changes (device, FALSE);
        devkit_disks_daemon_local_update_poller (device->priv->daemon);
}

static void
devkit_disks_device_drive_inhibit_polling_authorized_cb (DevkitDisksDaemon     *daemon,
                                                         DevkitDisksDevice     *device,
                                                         DBusGMethodInvocation *context,
                                                         const gchar           *action_id,
                                                         guint                  num_user_data,
                                                         gpointer              *user_data_elements)
{
        gchar **options = user_data_elements[0];
        DevkitDisksInhibitor *inhibitor;
        guint n;

        for (n = 0; options[n] != NULL; n++) {
                const char *option = options[n];
                throw_error (context,
                             DEVKIT_DISKS_ERROR_INVALID_OPTION,
                             "Unknown option %s", option);
                goto out;
        }

        inhibitor = devkit_disks_inhibitor_new (context);

        device->priv->polling_inhibitors = g_list_prepend (device->priv->polling_inhibitors, inhibitor);
        g_signal_connect (inhibitor, "disconnected", G_CALLBACK (polling_inhibitor_disconnected_cb), device);

        update_info (device);
        drain_pending_changes (device, FALSE);
        devkit_disks_daemon_local_update_poller (device->priv->daemon);

        dbus_g_method_return (context, devkit_disks_inhibitor_get_cookie (inhibitor));

out:
        ;
}

gboolean
devkit_disks_device_drive_inhibit_polling (DevkitDisksDevice     *device,
                                           char                 **options,
                                           DBusGMethodInvocation *context)
{
        if (!device->priv->device_is_drive) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a drive");
                goto out;
        }

        if (!device->priv->device_is_media_change_detection_inhibitable) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Media detection cannot be inhibited");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              "org.freedesktop.devicekit.disks.inhibit-polling",
                                              "DriveInhibitPolling",
                                              TRUE,
                                              devkit_disks_device_drive_inhibit_polling_authorized_cb,
                                              context,
                                              1,
                                              g_strdupv (options), g_strfreev);


 out:
        return TRUE;
}


/*--------------------------------------------------------------------------------------------------------------*/

gboolean
devkit_disks_device_drive_uninhibit_polling (DevkitDisksDevice     *device,
                                             char                  *cookie,
                                             DBusGMethodInvocation *context)
{
        const gchar *sender;
        DevkitDisksInhibitor *inhibitor;
        GList *l;

        sender = dbus_g_method_get_sender (context);

        inhibitor = NULL;
        for (l = device->priv->polling_inhibitors; l != NULL; l = l->next) {
                DevkitDisksInhibitor *i = DEVKIT_DISKS_INHIBITOR (l->data);

                if (g_strcmp0 (devkit_disks_inhibitor_get_unique_dbus_name (i), sender) == 0 &&
                    g_strcmp0 (devkit_disks_inhibitor_get_cookie (i), cookie) == 0) {
                        inhibitor = i;
                        break;
                }
        }

        if (inhibitor == NULL) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "No such inhibitor");
                goto out;
        }

        device->priv->polling_inhibitors = g_list_remove (device->priv->polling_inhibitors, inhibitor);
        g_object_unref (inhibitor);

        update_info (device);
        drain_pending_changes (device, FALSE);
        devkit_disks_daemon_local_update_poller (device->priv->daemon);

        dbus_g_method_return (context);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
drive_poll_media_completed_cb (DBusGMethodInvocation *context,
                               DevkitDisksDevice *device,
                               gboolean job_was_cancelled,
                               int status,
                               const char *stderr,
                               const char *stdout,
                               gpointer user_data)
{
        if (WEXITSTATUS (status) == 0 && !job_was_cancelled) {

                devkit_disks_device_generate_kernel_change_event (device);

                dbus_g_method_return (context);
        } else {
                if (job_was_cancelled) {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_CANCELLED,
                                     "Job was cancelled");
                } else {
                        throw_error (context,
                                     DEVKIT_DISKS_ERROR_FAILED,
                                     "Error detaching: helper exited with exit code %d: %s",
                                     WEXITSTATUS (status),
                                     stderr);
                }
        }
}

static void
devkit_disks_device_drive_poll_media_authorized_cb (DevkitDisksDaemon     *daemon,
                                                    DevkitDisksDevice     *device,
                                                    DBusGMethodInvocation *context,
                                                    const gchar           *action_id,
                                                    guint                  num_user_data,
                                                    gpointer              *user_data_elements)
{
        int n;
        char *argv[16];

        n = 0;
        argv[n++] = PACKAGE_LIBEXEC_DIR "/devkit-disks-helper-drive-poll";
        argv[n++] = device->priv->device_file;
        argv[n++] = NULL;

        if (!job_new (context,
                      "DrivePollMedia",
                      FALSE,
                      device,
                      argv,
                      NULL,
                      drive_poll_media_completed_cb,
                      NULL,
                      NULL)) {
                goto out;
        }

 out:
        ;
}

gboolean
devkit_disks_device_drive_poll_media (DevkitDisksDevice     *device,
                                      DBusGMethodInvocation *context)
{
        if (!device->priv->device_is_drive) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a drive");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              "org.freedesktop.devicekit.disks.inhibit-polling",
                                              "DrivePollMedia",
                                              TRUE,
                                              devkit_disks_device_drive_poll_media_authorized_cb,
                                              context,
                                              0);
 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/

static void
spindown_inhibitor_disconnected_cb (DevkitDisksInhibitor *inhibitor,
                                    DevkitDisksDevice   *device)
{
        device->priv->spindown_inhibitors = g_list_remove (device->priv->spindown_inhibitors, inhibitor);
        g_signal_handlers_disconnect_by_func (inhibitor, spindown_inhibitor_disconnected_cb, device);
        g_object_unref (inhibitor);

        update_info (device);
        drain_pending_changes (device, FALSE);
        devkit_disks_daemon_local_update_spindown (device->priv->daemon);
}

static void
devkit_disks_device_drive_set_spindown_timeout_authorized_cb (DevkitDisksDaemon     *daemon,
                                                              DevkitDisksDevice     *device,
                                                              DBusGMethodInvocation *context,
                                                              const gchar           *action_id,
                                                              guint                  num_user_data,
                                                              gpointer              *user_data_elements)
{
        gint timeout_seconds = GPOINTER_TO_INT (user_data_elements[0]);
        gchar **options = user_data_elements[1];
        DevkitDisksInhibitor *inhibitor;
        guint n;

        if (!device->priv->device_is_drive) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a drive");
                goto out;
        }

        if (!device->priv->drive_can_spindown) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Cannot spindown device");
                goto out;
        }

        if (timeout_seconds < 1) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Timeout seconds must be at least 1");
                goto out;
        }

        for (n = 0; options[n] != NULL; n++) {
                const char *option = options[n];
                throw_error (context,
                             DEVKIT_DISKS_ERROR_INVALID_OPTION,
                             "Unknown option %s", option);
                goto out;
        }

        inhibitor = devkit_disks_inhibitor_new (context);

        g_object_set_data (G_OBJECT (inhibitor), "spindown-timeout-seconds", GINT_TO_POINTER (timeout_seconds));

        device->priv->spindown_inhibitors = g_list_prepend (device->priv->spindown_inhibitors, inhibitor);
        g_signal_connect (inhibitor, "disconnected", G_CALLBACK (spindown_inhibitor_disconnected_cb), device);

        update_info (device);
        drain_pending_changes (device, FALSE);
        devkit_disks_daemon_local_update_spindown (device->priv->daemon);

        dbus_g_method_return (context, devkit_disks_inhibitor_get_cookie (inhibitor));

out:
        ;
}

gboolean
devkit_disks_device_drive_set_spindown_timeout (DevkitDisksDevice     *device,
                                                int                    timeout_seconds,
                                                char                 **options,
                                                DBusGMethodInvocation *context)
{
        if (!device->priv->device_is_drive) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Device is not a drive");
                goto out;
        }

        if (!device->priv->drive_can_spindown) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Cannot spindown device");
                goto out;
        }

        if (timeout_seconds < 1) {
                throw_error (context, DEVKIT_DISKS_ERROR_FAILED,
                             "Timeout seconds must be at least 1");
                goto out;
        }

        devkit_disks_daemon_local_check_auth (device->priv->daemon,
                                              device,
                                              "org.freedesktop.devicekit.disks.drive-set-spindown",
                                              "DriveSetSpindownTimeout",
                                              TRUE,
                                              devkit_disks_device_drive_set_spindown_timeout_authorized_cb,
                                              context,
                                              2,
                                              GINT_TO_POINTER (timeout_seconds), NULL,
                                              g_strdupv (options), g_strfreev);


 out:
        return TRUE;
}


/*--------------------------------------------------------------------------------------------------------------*/

gboolean
devkit_disks_device_drive_unset_spindown_timeout (DevkitDisksDevice     *device,
                                                  char                  *cookie,
                                                  DBusGMethodInvocation *context)
{
        const gchar *sender;
        DevkitDisksInhibitor *inhibitor;
        GList *l;

        sender = dbus_g_method_get_sender (context);

        inhibitor = NULL;
        for (l = device->priv->spindown_inhibitors; l != NULL; l = l->next) {
                DevkitDisksInhibitor *i = DEVKIT_DISKS_INHIBITOR (l->data);

                if (g_strcmp0 (devkit_disks_inhibitor_get_unique_dbus_name (i), sender) == 0 &&
                    g_strcmp0 (devkit_disks_inhibitor_get_cookie (i), cookie) == 0) {
                        inhibitor = i;
                        break;
                }
        }

        if (inhibitor == NULL) {
                throw_error (context,
                             DEVKIT_DISKS_ERROR_FAILED,
                             "No such spindown configurator");
                goto out;
        }

        device->priv->spindown_inhibitors = g_list_remove (device->priv->spindown_inhibitors, inhibitor);
        g_object_unref (inhibitor);

        update_info (device);
        drain_pending_changes (device, FALSE);
        devkit_disks_daemon_local_update_spindown (device->priv->daemon);

        dbus_g_method_return (context);

 out:
        return TRUE;
}

/*--------------------------------------------------------------------------------------------------------------*/
