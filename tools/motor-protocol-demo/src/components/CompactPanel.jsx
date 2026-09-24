import { useEffect, useId, useState } from 'react';

// Folding keeps the panel mounted, including drafts and in-flight board results.
export function CompactPanel({ title, initiallyOpen = false, desktopInitiallyOpen = true, className = '', preferenceKey, children }) {
  const storageKey = `device-panel:${preferenceKey || className || title}`;
  const [open, setOpen] = useState(() => {
    try {
      const saved = sessionStorage.getItem(storageKey);
      if (saved !== null) return saved === 'open';
    } catch {
      // Storage may be unavailable in private browsing.
    }
    return window.matchMedia('(max-width: 1100px)').matches ? initiallyOpen : desktopInitiallyOpen;
  });
  const contentId = useId();

  useEffect(() => {
    try { sessionStorage.setItem(storageKey, open ? 'open' : 'closed'); } catch { /* Optional preference only. */ }
  }, [storageKey, open]);

  return <div className={`compact-panel ${className}`} data-open={open}>
    <button type="button" className="compact-panel__toggle" aria-expanded={open} aria-controls={contentId} onClick={() => setOpen(value => !value)}>
      <span>{title}</span><span className="compact-panel__state">{open ? '收起' : '展开'}</span>
    </button>
    <div id={contentId} className="compact-panel__body">{children}</div>
  </div>;
}
