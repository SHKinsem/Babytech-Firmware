import { useMemo } from 'react';

import { COMMAND_GROUPS, opcodeLabel, searchCommands } from '../protocol.js';
import { GlyphChevron, GlyphInfo, GlyphSearch } from './glyphs.jsx';

export function CommandLibrary({ query, onQueryChange, openGroups, onToggleGroup, selectedId, onSelect }) {
  const matches = useMemo(() => new Set(searchCommands(query).map((item) => item.id)), [query]);
  const searching = query.trim().length > 0;

  return (
    <section className="library" aria-label="指令库">
      <h2 className="panel__title library__title">指令库</h2>

      <div className="search">
        <GlyphSearch />
        <input
          className="search__input"
          type="search"
          value={query}
          placeholder="搜索名称 / 功能码"
          aria-label="搜索指令名称或功能码"
          onChange={(event) => onQueryChange(event.target.value)}
        />
      </div>

      <div className="library__list">
        {COMMAND_GROUPS.map((group) => {
          const items = group.items.filter((item) => matches.has(item.id));
          if (searching && items.length === 0) return null;
          const open = searching || openGroups[group.id];
          return (
            <div className="group" key={group.id}>
              <button
                type="button"
                className="group__head"
                aria-expanded={open}
                onClick={() => onToggleGroup(group.id)}
              >
                <GlyphChevron open={open} />
                <span className="group__name">{group.name}</span>
                <span className="group__count">{items.length}</span>
              </button>

              {open ? (
                <ul className="group__items">
                  {items.map((item) => (
                    <li key={item.id}>
                      <button
                        type="button"
                        className={`library__item${selectedId === item.id ? ' is-selected' : ''}`}
                        aria-current={selectedId === item.id ? 'true' : undefined}
                        onClick={() => onSelect(item.id)}
                      >
                        <span className="library__item-name">{item.name}</span>
                        <span className="library__item-code">{opcodeLabel(item)}</span>
                      </button>
                    </li>
                  ))}
                  {items.length === 0 ? <li className="group__empty">暂无指令</li> : null}
                </ul>
              ) : null}
            </div>
          );
        })}

        {searching && matches.size === 0 ? (
          <p className="library__empty">没有匹配的指令，可尝试功能码（如 F3、36）或英文名（如 torque）。</p>
        ) : null}
      </div>

      <p className="library__note">
        <GlyphInfo />
        <span>C 系列含限速或限流变体。</span>
      </p>
    </section>
  );
}
