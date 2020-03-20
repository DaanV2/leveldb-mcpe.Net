﻿using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;

namespace LevelDB {
    public static partial class LevelDBInterop {
        /// <summary>
        /// 
        /// </summary>
        static LevelDBInterop() {
            Assembly assembly = Assembly.GetExecutingAssembly();

            //Library provide should be the same platform/archetecture
            String folder = Path.GetDirectoryName(assembly.Location);
            String path = Path.Combine(folder, "LevelDB-MCPE.dll");

            if (!File.Exists(path)) {
                if (Environment.Is64BitProcess) {
                    Copy(Path.Combine(folder, "LevelDB-MCPE-x64.dll"), path);
                }
                else {
                    Copy(Path.Combine(folder, "LevelDB-MCPE-x86.dll"), path);
                }
            }

            IntPtr h = LoadLibrary(path);

            if (h == IntPtr.Zero) {
                Int32 Value = Marshal.GetLastWin32Error();
                throw new ApplicationException($"Cannot load LevelDB-MCPE.dll, Error Code: {Value}");
            }
        }

        /// <summary>
        /// 
        /// </summary>
        /// <param name="source"></param>
        /// <param name="destination"></param>
        private static void Copy(String source, String destination) {
            if (!File.Exists(source)) {
                throw new ApplicationException($"Cannot find: {source}");
            }

            File.Copy(source, destination, true);
        }
    }
}
