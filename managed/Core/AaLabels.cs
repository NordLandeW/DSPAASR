using System;

namespace DSPAAMod.Core
{
    public static class AaLabels
    {
        // Native Chinese labels contain the localized long name plus an acronym,
        // e.g. 多重采样抗锯齿 (MSAA); neither the key nor the text is just "MSAA".
        public static bool Matches(string value, string acronym)
        {
            if (string.IsNullOrWhiteSpace(value)) return false;
            value = value.Trim();
            return value.Equals(acronym, StringComparison.OrdinalIgnoreCase) || value.EndsWith("(" + acronym + ")", StringComparison.OrdinalIgnoreCase) ||
                value.EndsWith("（" + acronym + "）", StringComparison.OrdinalIgnoreCase);
        }
    }
}
