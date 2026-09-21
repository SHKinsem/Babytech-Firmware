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

/** Small disclosure used instead of long paragraphs under every control. */
export function HelpTip({ label, text }) {
  return (
    <button
      type="button"
      className="help-tip"
      title={text}
      aria-label={`${label}：${text}`}
    >
      <Info size={12} weight="regular" aria-hidden="true" />
    </button>
  );
}
