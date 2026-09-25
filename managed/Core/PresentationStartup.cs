using System;
using System.IO;

namespace DSPAAMod.Core
{
    public enum PresentationStartupState { Missing, Inactive, Started, Failed }

    // This source is compiled into both assemblies. Only system primitives cross
    // the AppDomain boundary; the ordinary plugin never loads the patcher assembly.
    public sealed class PresentationStartup
    {
        private const string ReceiptKey = "DSPAASR.PresentationStartup";
        private const int ReceiptVersion = 1;
        public PresentationStartupState State { get; }
        public string RuntimeDirectory { get; }
        // Native/config backend identity: Off=0, Fsr=1, Dlss=2; -1 means unknown.
        public int RequestedBackend { get; }
        public string Error { get; }

        private PresentationStartup(PresentationStartupState state, string directory, int backend, string error)
        { State = state; RuntimeDirectory = directory; RequestedBackend = backend; Error = error ?? string.Empty; }

        public static PresentationStartup Read(string runtimeDirectory)
        {
            string expected;
            try { expected = NormalizeDirectory(runtimeDirectory); }
            catch (Exception error) { return Failure(string.Empty, "Invalid plugin directory: " + error.Message); }
            object raw;
            lock (AppDomain.CurrentDomain) raw = AppDomain.CurrentDomain.GetData(ReceiptKey);
            if (raw == null) return new PresentationStartup(PresentationStartupState.Missing, expected, -1,
                "The presentation preloader did not report a result in this process.");
            var receipt = raw as object[];
            if (receipt == null || receipt.Length != 5 || !(receipt[0] is int version) || version != ReceiptVersion ||
                !(receipt[1] is int state) || state < (int)PresentationStartupState.Inactive || state > (int)PresentationStartupState.Failed ||
                !(receipt[2] is string directory) || !(receipt[3] is int backend) || backend < -1 || backend > 2 ||
                !(receipt[4] is string errorText))
                return Failure(expected, "Invalid presentation preloader receipt.");
            var result = (PresentationStartupState)state;
            if ((result == PresentationStartupState.Inactive && backend != 0) ||
                (result == PresentationStartupState.Started && backend != 1 && backend != 2))
                return Failure(expected, "Inconsistent presentation preloader intent.");
            if (!string.Equals(directory, expected, StringComparison.OrdinalIgnoreCase))
            {
                // Failures before selecting a unique payload still have useful
                // diagnostics, but can never grant the normal inactive state.
                if (result == PresentationStartupState.Failed && directory.Length == 0)
                    return new PresentationStartup(result, expected, backend, errorText);
                return Failure(expected, "The presentation preloader selected a different plugin directory.");
            }
            return new PresentationStartup(result, directory, backend, errorText);
        }

        private static PresentationStartup Failure(string directory, string error) =>
            new PresentationStartup(PresentationStartupState.Failed, directory, -1, error);

        internal static string NormalizeDirectory(string directory)
        {
            if (string.IsNullOrWhiteSpace(directory)) throw new ArgumentException("A plugin directory is required.");
            string full = Path.GetFullPath(directory);
            string root = Path.GetPathRoot(full);
            return full.Length > root.Length ? full.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) : full;
        }

        internal static bool TryClaim()
        {
            // This lock is shared even when two copies of the patcher assembly
            // were discovered. Failed/partial initialization is never retried.
            lock (AppDomain.CurrentDomain)
            {
                if (AppDomain.CurrentDomain.GetData(ReceiptKey) != null) return false;
                AppDomain.CurrentDomain.SetData(ReceiptKey, new object[] { ReceiptVersion,
                    (int)PresentationStartupState.Failed, string.Empty, -1,
                    "Presentation preloader initialization did not complete." });
                return true;
            }
        }

        internal static void Publish(PresentationStartupState state, string directory, int backend, string error = "")
        {
            if (state == PresentationStartupState.Missing) throw new ArgumentOutOfRangeException(nameof(state));
            string normalized = string.IsNullOrEmpty(directory) ? string.Empty : NormalizeDirectory(directory);
            var receipt = new object[] { ReceiptVersion, (int)state, normalized, backend, error ?? string.Empty };
            lock (AppDomain.CurrentDomain) AppDomain.CurrentDomain.SetData(ReceiptKey, receipt);
        }
    }
}
