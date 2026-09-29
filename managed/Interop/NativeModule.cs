using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;

namespace DSPAAMod.Interop
{
    // Shared by the ordinary plugin and the early preloader; no Unity dependency.
    internal static class NativeModule
    {
        [DllImport("kernel32.dll", EntryPoint = "LoadLibraryExW", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        private static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);

        internal static IntPtr Load(string path)
        {
            path = Path.GetFullPath(path);
            // Search the explicit module directory and Windows' safe default directories.
            // Never fall back to a different copy of this module in the working directory.
            IntPtr module = LoadLibraryEx(path, IntPtr.Zero, 0x00000100 | 0x00001000);
            if (module == IntPtr.Zero)
            {
                int error = Marshal.GetLastWin32Error();
                throw new NativeModuleLoadException(error, path, File.Exists(path));
            }
            return module;
        }
    }

    internal sealed class NativeModuleLoadException : Win32Exception
    {
        internal string ModulePath { get; }
        internal bool ModuleFound { get; }

        internal NativeModuleLoadException(int error, string path, bool found)
            : base(error, Describe(error, found))
        {
            ModulePath = path;
            ModuleFound = found;
        }

        private static string Describe(int error, bool found)
        {
            string hint;
            if ((error == 2 || error == 3 || error == 126) && !found)
                hint = "Keep the complete package beside DSPAAMod.dll in the plugin folder.";
            else if (error == 126)
                hint = "The file exists; a required dependency may be missing.";
            else if (error == 193 || error == 216)
                hint = "Check for a damaged or non-x64 DLL and reinstall the complete package.";
            else
                hint = "Check the installation and Windows file access.";
            return "Cannot load DSPAANative.dll (Windows error " + error + ": " +
                new Win32Exception(error).Message + "). " + hint + " See BepInEx/LogOutput.log.";
        }

        public override string ToString() => base.ToString() + Environment.NewLine +
            "Expected native module: " + ModulePath + Environment.NewLine +
            "File present at diagnostic check: " + ModuleFound;
    }
}
