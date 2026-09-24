import { useEffect, useId, useRef, useState } from 'react';
// Icons come from @phosphor-icons/react so no icon is hand drawn. The per-icon
// entry points keep the dev graph and the production bundle small.

import { CaretDown } from '@phosphor-icons/react/dist/csr/CaretDown';
import { CaretRight } from '@phosphor-icons/react/dist/csr/CaretRight';
import { Circle } from '@phosphor-icons/react/dist/csr/Circle';
import { Info } from '@phosphor-icons/react/dist/csr/Info';
import { MagnifyingGlass } from '@phosphor-icons/react/dist/csr/MagnifyingGlass';

export function GlyphSearch({ size = 14 }) {
  return <MagnifyingGlass size={size} weight="regular" className="glyph glyph--search-icon" aria-hidden="true" />;
}

export function GlyphChevron({ open = false }) {
  const Icon = open ? CaretDown : CaretRight;
  return <Icon size={12} weight="bold" className="glyph glyph--chevron-icon" aria-hidden="true" />;
}

export function GlyphInfo({ size = 14 }) {
  return <Info size={size} weight="regular" className="glyph glyph--info-icon" aria-hidden="true" />;
}

export function GlyphDot({ tone = 'ok', size = 9 }) {
  return (
    <Circle
      size={size}
      weight="fill"
      className={`glyph glyph--dot glyph--dot-${tone}`}
      aria-hidden="true"
    />
  );
}

/** Tap, click and keyboard disclosure for field notes. */
export function HelpTip({ label, text }) {
  const [open, setOpen] = useState(false);
  const contentId = useId();
  const rootRef = useRef(null);
  useEffect(() => {
    if (!open) return undefined;
    const onKeyDown = event => { if (event.key === 'Escape') setOpen(false); };
    const onPointerDown = event => { if (!rootRef.current?.contains(event.target)) setOpen(false); };
    document.addEventListener('keydown', onKeyDown);
    document.addEventListener('pointerdown', onPointerDown);
    return () => {
      document.removeEventListener('keydown', onKeyDown);
      document.removeEventListener('pointerdown', onPointerDown);
    };
  }, [open]);
  return <span className="help-tip-wrap" ref={rootRef}>
    <button type="button" className="help-tip" aria-label={`查看${label}说明`} aria-expanded={open} aria-controls={contentId} aria-describedby={open ? contentId : undefined} onClick={() => setOpen(value => !value)}>
      <Info size={14} weight="regular" aria-hidden="true" />
    </button>
    {open ? <span id={contentId} className="help-tip__popover" role="note">{text}</span> : null}
  </span>;
}
