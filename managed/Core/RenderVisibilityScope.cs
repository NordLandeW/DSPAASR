using System;
using System.Collections.Generic;

namespace DSPAAMod.Core
{
    // Own only false -> true suppression changes. Restoration is idempotent and
    // keeps a failed item for retry, while still restoring all other live items.
    internal sealed class RenderVisibilityScope<T> where T : class
    {
        private readonly Func<T, bool> alive, suppressed;
        private readonly Action<T, bool> setSuppressed;
        private readonly List<T> changed = new List<T>();
        public RenderVisibilityScope(Func<T, bool> alive, Func<T, bool> suppressed, Action<T, bool> setSuppressed)
        { this.alive = alive; this.suppressed = suppressed; this.setSuppressed = setSuppressed; }
        public bool Suppress(T item)
        {
            if (item == null || !alive(item) || suppressed(item) || changed.Contains(item)) return false;
            changed.Add(item); // Retain ownership even if a setter changes state and then throws.
            setSuppressed(item, true);
            return true;
        }
        public void Restore()
        {
            Exception failure = null;
            for (int i = changed.Count - 1; i >= 0; --i)
            {
                try
                {
                    T item = changed[i];
                    if (alive(item) && suppressed(item)) setSuppressed(item, false);
                    changed.RemoveAt(i);
                }
                catch (Exception error) { if (failure == null) failure = error; }
            }
            if (failure != null) throw failure;
        }
    }
}
