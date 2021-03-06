﻿using System;

namespace LevelDB {
    /// <summary>
    /// Options that control read operations.
    /// </summary>
    public class ReadOptions : LevelDBHandle {
        /// <summary> </summary>
        public ReadOptions() {
            this.Handle = LevelDBInterop.leveldb_readoptions_create();
        }

        /// <summary>If true, all data read from underlying storage will be
        /// verified against corresponding checksums. </summary>
        public Boolean VerifyCheckSums {
            set => LevelDBInterop.leveldb_readoptions_set_verify_checksums(this.Handle, value ? (Byte)1 : (Byte)0);
        }

        /// <summary>Should the data read for this iteration be cached in memory?
        /// Callers may wish to set this field to false for bulk scans.
        /// Default: true </summary>
        public Boolean FillCache {
            set => LevelDBInterop.leveldb_readoptions_set_fill_cache(this.Handle, value ? (Byte)1 : (Byte)0);
        }

        /// <summary>If "snapshot" is provides, read as of the supplied snapshot
        /// (which must belong to the DB that is being read and which must
        /// not have been released).  
        /// If "snapshot" is not set, use an implicit
        /// snapshot of the state at the beginning of this read operation. </summary>
        public SnapShot Snapshot {
            set => LevelDBInterop.leveldb_readoptions_set_snapshot(this.Handle, value.Handle);
        }

        /// <summary> </summary>
        protected override void FreeUnManagedObjects() {
            LevelDBInterop.leveldb_readoptions_destroy(this.Handle);
        }
    }
}
