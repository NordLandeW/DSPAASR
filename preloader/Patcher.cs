using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using BepInEx;
using BepInEx.Configuration;
using BepInEx.Logging;
using DSPAAMod.Core;
using Mono.Cecil;

namespace DSPAASR.Preloader
{
    // BepInEx requires both members to discover an initialization-only patcher.
    // No Unity/game assembly is referenced or loaded from this entry point.
    public static class Patcher
    {
        public static IEnumerable<string> TargetDLLs => Array.Empty<string>();
        public static void Patch(AssemblyDefinition assembly) { }

        public static void Initialize()
        {
            if (!PresentationStartup.TryClaim()) return;
            string runtime = string.Empty;
            int backend = -1;
            var log = Logger.CreateLogSource("DSPAASR.Preloader");
            try
            {
                if (Environment.OSVersion.Platform != PlatformID.Win32NT || !Environment.Is64BitProcess ||
                    !string.Equals(Path.GetFileName(Paths.ExecutablePath), "DSPGAME.exe", StringComparison.OrdinalIgnoreCase))
                    throw new PlatformNotSupportedException("DSPAASR presentation requires the Windows x64 DSPGAME process.");
                runtime = FindRuntimeDirectory(Paths.PluginPath);
                backend = ReadBackend(Path.Combine(Paths.ConfigPath, "dspaa.mod.cfg"));
                if (backend == 0)
                {
                    PresentationStartup.Publish(PresentationStartupState.Inactive, runtime, backend);
                    log.LogInfo("Frame generation is off at startup; this preloader did not initialize the presentation bridge.");
                    return;
                }

                StartPresentation(runtime, Path.Combine(Paths.CachePath, "DSPAAMod"));
                // This confirms only the early entry. The ordinary plugin must
                // query its real channel before promising live FG capabilities.
                PresentationStartup.Publish(PresentationStartupState.Started, runtime, backend);
                log.LogInfo("Early presentation entry installed; awaiting the game's presentation channel.");
            }
            catch (Exception error)
            {
                PresentationStartup.Publish(PresentationStartupState.Failed, runtime, backend, error.Message);
                log.LogError("Presentation startup failed; no retry or native unload will be attempted: " + error);
            }
        }

        private static string FindRuntimeDirectory(string pluginPath)
        {
            string root = PresentationStartup.NormalizeDirectory(pluginPath);
            if (!Directory.Exists(root)) throw new DirectoryNotFoundException("The active profile has no plugin directory.");
            string managed = null;
            foreach (string candidate in Directory.GetFiles(root, "DSPAAMod.dll", SearchOption.AllDirectories))
            {
                if (!string.Equals(Path.GetFileName(candidate), "DSPAAMod.dll", StringComparison.OrdinalIgnoreCase)) continue;
                if (managed != null) throw new InvalidOperationException("Multiple enabled DSPAASR plugin installations were found in this profile.");
                managed = candidate;
            }
            if (managed == null) throw new FileNotFoundException("The DSPAASR plugin is disabled or absent from this profile.");
            // Metadata-only inspection: never load the Unity-dependent plugin
            // assembly while BepInEx is still preparing its patched assemblies.
            var identity = AssemblyName.GetAssemblyName(managed);
            var ownVersion = typeof(Patcher).Assembly.GetName().Version;
            if (identity.Name != "DSPAAMod" || identity.Version != ownVersion)
                throw new InvalidOperationException("The DSPAASR plugin and preloader versions do not match.");
            string runtime = PresentationStartup.NormalizeDirectory(Path.GetDirectoryName(managed));
            if (!File.Exists(Path.Combine(runtime, "DSPAANative.dll")))
                throw new FileNotFoundException("DSPAANative.dll is disabled or missing beside the active DSPAASR plugin.");
            return runtime;
        }

        private static int ReadBackend(string path)
        {
            var config = new ConfigFile(path, false) { SaveOnConfigSet = false };
            // Binding an enum would silently replace an invalid saved value with
            // Off. Read the string so a malformed intent becomes a real failure.
            string value = config.Bind<string>("FrameGeneration", "Backend", nameof(FrameGenerationBackend.Off)).Value;
            // ConfigFile already trims source values. Reject whitespace produced
            // by string unescaping, which the plugin's enum converter rejects.
            if (value != value.Trim() || !Enum.TryParse(value, true, out FrameGenerationBackend backend) ||
                !Enum.IsDefined(typeof(FrameGenerationBackend), backend))
                throw new InvalidDataException("Unknown saved FrameGeneration.Backend: " + value);
            return (int)backend;
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private delegate int Bootstrap(uint abi, [MarshalAs(UnmanagedType.LPWStr)] string runtime,
            [MarshalAs(UnmanagedType.LPWStr)] string data, uint mode);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate uint GetAbi();
        [DllImport("kernel32.dll", EntryPoint = "LoadLibraryExW", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        private static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
        [DllImport("kernel32.dll", EntryPoint = "GetModuleHandleExW", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetModuleHandleEx(uint flags, string path, out IntPtr module);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);

        private static T Export<T>(IntPtr module, string name) where T : Delegate
        {
            IntPtr address = GetProcAddress(module, name);
            if (address == IntPtr.Zero) throw new EntryPointNotFoundException("Missing DSPAANative export: " + name);
            return (T)Marshal.GetDelegateForFunctionPointer(address, typeof(T));
        }

        private static void StartPresentation(string runtime, string data)
        {
            string path = Path.Combine(runtime, "DSPAANative.dll");
            IntPtr module = LoadLibraryEx(path, IntPtr.Zero, 0x00000100 | 0x00001000);
            if (module == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot load DSPAANative.dll.");
            // Native callbacks/trampolines can outlive the patcher and any error.
            // Keep both this load reference and a process-lifetime pin; never free.
            if (!GetModuleHandleEx(0x00000001, path, out _))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot pin DSPAANative.dll.");
            if (Export<GetAbi>(module, "DspAaGetAbiVersion")() != 2 ||
                Export<GetAbi>(module, "DspAaGetPresentationAbiVersion")() != 3)
                throw new InvalidOperationException("The installed native DSPAASR ABI does not match this preloader.");
            if (Export<Bootstrap>(module, "DspAaBootstrapPresentation")(1, runtime, data, 1) != 1)
                throw new InvalidOperationException("Native early presentation initialization was rejected.");
        }
    }
}
