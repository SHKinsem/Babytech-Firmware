const SCALED_UNITS = {
  '0.1 RPM': { label: 'RPM', scale: 10 },
  '0.1°': { label: '°', scale: 10 },
};

export function displayUnit(field) {
  return SCALED_UNITS[field?.unit]?.label ?? field?.unit ?? '';
}

export function displayValue(field, raw) {
  const scale = SCALED_UNITS[field?.unit]?.scale;
  const text = String(raw ?? '');
  if (!scale || !text.trim()) return text;
  if (!/^\d+$/.test(text)) return text;
  const number = Number(text);
  return Number.isSafeInteger(number) ? String(number / scale) : text;
}

export function protocolValue(field, displayed) {
  const scale = SCALED_UNITS[field?.unit]?.scale;
  if (!scale) return displayed;
  const text = String(displayed);
  const match = /^(?:(\d+)(?:\.(\d))?|\.(\d))$/.exec(text);
  if (!match) return displayed;
  const whole = Number(match[1] ?? 0);
  const tenth = Number(match[2] || match[3] || 0);
  const raw = whole * scale + tenth;
  return Number.isSafeInteger(raw) ? String(raw) : displayed;
}

export function displayRange(field) {
  const scale = SCALED_UNITS[field?.unit]?.scale;
  if (!scale) return `${field.min}–${field.max} · ${field.bytes} 字节大端`;
  return `${field.min / scale}–${field.max / scale} ${displayUnit(field)} · 自动换算`;
}

export function displayError(field, displayed, protocolError) {
  if (!protocolError || !SCALED_UNITS[field?.unit]) return protocolError;
  const text = String(displayed).trim();
  if (!text) return `${field.label}不能为空`;
  if (!/^(?:(\d+)(?:\.(\d))?|\.(\d))$/.test(text)) {
    return `${field.label}最多填写 1 位小数（${displayUnit(field)}）`;
  }
  const number = Number(text);
  const scale = SCALED_UNITS[field.unit].scale;
  if (!Number.isFinite(number) || number < field.min / scale || number > field.max / scale) {
    return `${field.label}需在 ${field.min / scale}–${field.max / scale} ${displayUnit(field)} 之间`;
  }
  return protocolError;
}
