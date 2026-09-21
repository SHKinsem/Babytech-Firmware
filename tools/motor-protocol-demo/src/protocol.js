// Pure, framework-free X42S / X28S protocol model.
//
// Sources of truth (see references/):
//   X42sProtocol.cpp / .h  byte layout of every command and the sendCommand()
//                          packet split.
//   x42s_can_id.h          address in bits 8..15, packet index in bits 0..7.
//   MotionCore.h           wire units (0.1 degree, 0.1 RPM, mA, whole RPM/s),
//                          ack status bytes and the trapezoid duration math.
//
// Nothing here touches React, timers or the network. Every exported encoder is
// deterministic so it can be unit tested byte for byte.

export const PROTOCOL_CHECKSUM = 0x6b; // every command ends with the fixed 0x6B
export const ADDRESS_MIN = 1;
export const ADDRESS_MAX = 255;
export const PAYLOAD_BYTES_PER_FRAME = 7; // sendCommand: 7 payload bytes per frame
export const CURRENT_MA_MAX = 5000; // X42sProtocol::constrainCurrentMa upper bound
export const LOGICAL_COMMAND_MAX_BYTES = 255; // sendCommand's uint8_t len

// MotionCore.h wire scaling.
export const TENTHS_PER_DEGREE = 10;
export const TENTHS_PER_RPM = 10;

// MotionCore.h accepted request ranges (used by the 常规试动 tab).
export const MANUAL_LIMITS = {
  minAbsAngleDeg: 0.1,
  maxAbsAngleDeg: 3600,
  minSpeedRpm: 0.1,
  maxSpeedRpm: 120,
  minAccelRpmS: 1,
  maxAccelRpmS: 240,
  minCurrentMa: 100,
  maxCurrentMa: 5000,
  maxExpectedDurationMs: 60000,
};

// How well a value is established by the copied firmware sources.
export const CERTAINTY = {
  grounded: '源码明确',
  name: '参数名推断',
  protocol: '协议字段',
};

export const certaintyLabel = (key) => CERTAINTY[key] ?? CERTAINTY.protocol;

// ---------------------------------------------------------------------------
// Field / layout helpers
// ---------------------------------------------------------------------------

const constant = (value, label) => ({ kind: 'const', value, label });

function intField(key, label, bytes, extra = {}) {
  return {
    kind: 'field',
    key,
    label,
    bytes,
    type: 'int',
    control: 'input',
    min: 0,
    max: 2 ** (bytes * 8) - 1,
    default: 0,
    certainty: 'protocol',
    ...extra,
  };
}

function enumField(key, label, options, extra = {}) {
  return {
    kind: 'field',
    key,
    label,
    bytes: 1,
    type: 'enum',
    control: 'segmented',
    options,
    default: extra.default ?? options[0].value,
    certainty: 'protocol',
    ...extra,
  };
}

const DIRECTION_OPTIONS = [
  { value: 0, label: '正向' },
  { value: 1, label: '反向' },
];

const SAVE_OPTIONS = [
  { value: 1, label: '保存' },
  { value: 0, label: '不保存' },
];

const ON_OFF_OPTIONS = [
  { value: 1, label: '开启' },
  { value: 0, label: '关闭' },
];

const SYNC_FIELD = () =>
  enumField('sync', '同步执行', [
    { value: 0, label: '关闭' },
    { value: 1, label: '等待触发' },
  ], {
    default: 0,
    byteLabel: '同步',
    certainty: 'name',
    hint: '参数名 sync。0 立即提交，非 0 交给 FF（同步触发）执行；网页侧按模拟队列处理。',
  });

const CURRENT_LIMIT_FIELD = (key = 'maxCurrentMa') =>
  intField(key, '电流上限', 2, {
    unit: 'mA',
    max: CURRENT_MA_MAX,
    default: 1000,
    certainty: 'grounded',
    hint: 'mA。固件 constrainCurrentMa 会把越界值截断到 0..5000，本工具改为直接报错，不做静默截断。',
  });

// ---------------------------------------------------------------------------
// Command variants
//
// A "variant" is one real firmware function code (for example F5 and C5 are the
// plain and current-limited torque commands). Items that have two variants are
// shown once in the library with both codes, exactly like the reference design.
// ---------------------------------------------------------------------------

const V = (key, label, opcode, layout, extra = {}) => ({ key, label, opcode, layout, ...extra });

const READ_ITEMS = [
  { key: 'readVer', opcode: 0x1f, name: '版本 Ver', en: 'Ver version', certainty: 'protocol', meaning: '固件枚举名 Ver；应答数据布局未在源码中定义。' },
  { key: 'readRl', opcode: 0x20, name: 'Rl（协议字段）', en: 'Rl', certainty: 'protocol', meaning: '固件枚举名 Rl；含义与单位未在源码中定义。' },
  { key: 'readPid', opcode: 0x21, name: 'PID 参数', en: 'Pid', certainty: 'protocol', meaning: '固件枚举名 Pid；数据布局未在源码中定义。' },
  { key: 'readVbus', opcode: 0x24, name: '母线电压 Vbus', en: 'Vbus voltage', certainty: 'protocol', meaning: '固件枚举名 Vbus；单位未在源码中定义。' },
  { key: 'readCpha', opcode: 0x27, name: '相电流 (mA)', en: 'Cpha current mA', certainty: 'grounded', meaning: '0x27 电流帧：[0x27][mA15..8][mA7..0][0x6B]（MotionCore.h）。' },
  { key: 'readEncl', opcode: 0x31, name: 'Encl（协议字段）', en: 'Encl encoder', certainty: 'protocol', meaning: '固件枚举名 Encl；含义与单位未在源码中定义。' },
  { key: 'readTpos', opcode: 0x33, name: '目标位置 Tpos', en: 'Tpos target position', certainty: 'protocol', meaning: '固件枚举名 Tpos；单位未在源码中定义（可参照 0x36 的 0.1° 约定）。' },
  { key: 'readVel', opcode: 0x35, name: '速度 (0.1 RPM)', en: 'Vel velocity RPM', certainty: 'grounded', meaning: '0x35 速度帧：[0x35][sign][mag15..8][mag7..0][0x6B]，单位 0.1 RPM（MotionCore.h）。' },
  { key: 'readCpos', opcode: 0x36, name: '当前位置 (0.1°)', en: 'Cpos current position', certainty: 'grounded', meaning: '0x36 位置帧：[0x36][sign][mag31..24..0][0x6B]，单位 0.1°（MotionCore.h）。' },
  { key: 'readPerr', opcode: 0x37, name: '位置误差 Perr', en: 'Perr position error', certainty: 'protocol', meaning: '固件枚举名 Perr；单位未在源码中定义。' },
  { key: 'readFlag', opcode: 0x3a, name: '状态标志 Flag', en: 'Flag status bits', certainty: 'grounded', meaning: '0x3A 标志帧：[0x3A][flags][0x6B]，位定义未在源码中定义。' },
  { key: 'readOrg', opcode: 0x3b, name: 'Org（协议字段）', en: 'Org origin', certainty: 'protocol', meaning: '固件枚举名 Org；含义与单位未在源码中定义。' },
  { key: 'readConf', opcodes: [0x42, 0x6c], name: '配置读取 Conf', en: 'Conf config', certainty: 'protocol', meaning: '固件枚举名 Conf，逻辑指令为 [addr][0x42][0x6C][0x6B]。' },
  { key: 'readState', opcodes: [0x43, 0x7a], name: '状态读取 State', en: 'State status', certainty: 'protocol', meaning: '固件枚举名 State，逻辑指令为 [addr][0x43][0x7A][0x6B]。' },
];

const readVariant = (entry) => {
  const codes = entry.opcodes ?? [entry.opcode];
  return V(
    'base',
    '读取',
    codes[0],
    [...codes.slice(1).map((value, index) => constant(value, index === 0 ? '子功能' : '参数')), constant(PROTOCOL_CHECKSUM, '固定校验')],
  );
};

export const COMMAND_GROUPS = [
  {
    id: 'basic',
    name: '基础控制',
    open: true,
    items: [
      {
        id: 'enable',
        name: '使能/失能',
        en: 'enable disable F3',
        sendNote: '使能请求',
        integrated: true,
        summary: '控制电机使能状态，可选择等待同步触发。',
        variants: [
          V('base', 'F3', 0xf3, [
            constant(0xab, '标识'),
            enumField('state', '使能状态', [
              { value: 1, label: '使能' },
              { value: 0, label: '失能' },
            ], { default: 1, certainty: 'grounded', hint: '源码 enableControl(addr, state, sync) 按 0/1 透传。' }),
            SYNC_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '同步选项：驱动已支持；网页侧为模拟排队，需发送 FF（同步触发）才会执行。',
      },
      {
        id: 'stop',
        name: '立即停止',
        en: 'stop now FE',
        sendNote: '停止请求',
        integrated: true,
        summary: '立即停止当前运动，可选择等待同步触发。',
        variants: [
          V('base', 'FE', 0xfe, [
            constant(0x98, '标识'),
            SYNC_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '模拟中停止会同时作废未完成的运动与待触发队列，不会再有迟到的完成记录。',
      },
      {
        id: 'syncTrigger',
        name: '同步触发',
        en: 'synchronous motion trigger FF',
        sendNote: '同步触发',
        integrated: true,
        summary: '执行此前以“等待触发”方式提交的指令。',
        variants: [
          V('base', 'FF', 0xff, [constant(0x66, '标识'), constant(PROTOCOL_CHECKSUM, '固定校验')]),
        ],
        note: '网页侧只执行模拟队列；无队列时仅发送帧。',
      },
    ],
  },
  {
    id: 'motion',
    name: '运动控制',
    open: true,
    items: [
      {
        id: 'torque',
        name: '力矩',
        en: 'torque current F5 C5 maxSpeed',
        sendNote: '力矩请求',
        integrated: true,
        summary: '电流闭环力矩控制，可选限速变体（C 系列）。',
        variants: [
          V('base', 'F5 基础', 0xf5, [
            enumField('dir', '方向', DIRECTION_OPTIONS, { default: 0, certainty: 'grounded', hint: '0 正向，1 反向（MotionCore kDirectionPositive/kDirectionNegative）。' }),
            intField('accel', '电流加速度', 2, { unit: 'mA/s', default: 1000, max: 65535, certainty: 'name', hint: '参数名 accelMaPerSec（mA/s），源码未给出取值范围。' }),
            CURRENT_LIMIT_FIELD('currentMa'),
            SYNC_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
          V('limit', 'C5 限速', 0xc5, [
            enumField('dir', '方向', DIRECTION_OPTIONS, { default: 0, certainty: 'grounded', hint: '0 正向，1 反向。' }),
            intField('accel', '电流加速度', 2, { unit: 'mA/s', default: 1000, max: 65535, certainty: 'name', hint: '参数名 accelMaPerSec（mA/s）。' }),
            CURRENT_LIMIT_FIELD('currentMa'),
            SYNC_FIELD(),
            intField('maxSpeed', '限速', 2, { default: 0, max: 65535, certainty: 'protocol', hint: '参数名 maxSpeed；单位未在源码中定义，按原始 16 位大端发送。' }),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '力矩指令按电流（mA）下发，不是扭矩 Nm：源码只暴露电流值。',
      },
      {
        id: 'velocity',
        name: '速度',
        en: 'velocity speed F6 C6 current limit',
        sendNote: '速度请求',
        integrated: true,
        summary: '速度闭环控制，可选限流变体（C 系列）。',
        variants: [
          V('base', 'F6 基础', 0xf6, [
            enumField('dir', '方向', DIRECTION_OPTIONS, { default: 0, certainty: 'grounded', hint: '0 正向，1 反向。' }),
            intField('acc', '加速度', 2, { default: 0, max: 65535, certainty: 'protocol', hint: '参数名 acc；单位未在源码中定义（位置指令的加减速为整数 RPM/s）。' }),
            intField('vel', '速度', 2, { default: 600, max: 65535, certainty: 'grounded', hint: '0.1 RPM（MotionCore kTenthsPerRpm）。600 = 60.0 RPM。' }),
            SYNC_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
          V('limit', 'C6 限流', 0xc6, [
            enumField('dir', '方向', DIRECTION_OPTIONS, { default: 0, certainty: 'grounded', hint: '0 正向，1 反向。' }),
            intField('acc', '加速度', 2, { default: 0, max: 65535, certainty: 'protocol', hint: '参数名 acc；单位未在源码中定义。' }),
            intField('vel', '速度', 2, { default: 600, max: 65535, certainty: 'grounded', hint: '0.1 RPM。' }),
            SYNC_FIELD(),
            CURRENT_LIMIT_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '速度模式为连续运动：演示中最多模拟 10 秒，随后自动停转（模拟限制）。',
      },
      {
        id: 'passthroughPosition',
        name: '直通位置',
        en: 'passthrough position clk FB CB',
        sendNote: '直通位置请求',
        integrated: true,
        summary: '直通位置控制，速度与行程由驱动器内部规划。',
        variants: [
          V('base', 'FB 基础', 0xfb, [
            enumField('dir', '方向', DIRECTION_OPTIONS, { default: 0, certainty: 'grounded', hint: '0 正向，1 反向。' }),
            intField('vel', '速度', 2, { default: 600, max: 65535, certainty: 'protocol', hint: '参数名 vel；固件未说明与 0.1 RPM 的换算关系。' }),
            intField('clk', '行程', 4, { default: 1800, max: 0xffffffff, certainty: 'protocol', hint: '参数名 clk（uint32）；直通模式脉冲数，固件未定义单位。' }),
            intField('motionMode', '运动模式', 1, { default: 2, max: 255, certainty: 'protocol', hint: '协议字段：固件未定义取值表；MotionCore 中 2 = kMotionModeRelativeToCurrent。' }),
            SYNC_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
          V('limit', 'CB 限流', 0xcb, [
            enumField('dir', '方向', DIRECTION_OPTIONS, { default: 0, certainty: 'grounded', hint: '0 正向，1 反向。' }),
            intField('vel', '速度', 2, { default: 600, max: 65535, certainty: 'protocol', hint: '参数名 vel；单位未在源码中定义。' }),
            intField('clk', '行程', 4, { default: 1800, max: 0xffffffff, certainty: 'protocol', hint: '参数名 clk（uint32）；单位未在源码中定义。' }),
            intField('motionMode', '运动模式', 1, { default: 2, max: 255, certainty: 'protocol', hint: '协议字段。' }),
            SYNC_FIELD(),
            CURRENT_LIMIT_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '演示按 0.1° 约定折算 clk，仅为可视化；固件未定义直通模式的行程单位。',
      },
      {
        id: 'position',
        name: '梯形位置',
        en: 'trapezoid position FD CD accel decel',
        sendNote: '位置请求',
        integrated: true,
        summary: '梯形加减速位置控制，可选限流变体（C 系列）。',
        variants: [
          V('base', 'FD 基础', 0xfd, [
            enumField('dir', '方向', DIRECTION_OPTIONS, { default: 0, certainty: 'grounded', hint: '0 正向，1 反向。' }),
            intField('accel', '加速度', 2, { unit: 'RPM/s', default: 60, max: 65535, certainty: 'grounded', hint: '整数 RPM/s，无 ×10 缩放（MotionCore buildMovePlan）。' }),
            intField('decel', '减速度', 2, { unit: 'RPM/s', default: 60, max: 65535, certainty: 'grounded', hint: '整数 RPM/s。' }),
            intField('vel', '速度', 2, { default: 600, max: 65535, certainty: 'grounded', hint: '0.1 RPM。600 = 60.0 RPM。' }),
            intField('clk', '行程', 4, { default: 1800, max: 0xffffffff, certainty: 'grounded', hint: '0.1° 行程幅值，方向由 dir 决定（MotionCore 位置指令）。1800 = 180.0°。' }),
            intField('motionMode', '运动模式', 1, { default: 2, max: 255, certainty: 'protocol', hint: '协议字段；示例默认 2（kMotionModeRelativeToCurrent）。' }),
            SYNC_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
          V('limit', 'CD 限流', 0xcd, [
            enumField('dir', '方向', DIRECTION_OPTIONS, { default: 0, certainty: 'grounded', hint: '0 正向，1 反向。' }),
            intField('accel', '加速度', 2, { unit: 'RPM/s', default: 60, max: 65535, certainty: 'grounded', hint: '整数 RPM/s。' }),
            intField('decel', '减速度', 2, { unit: 'RPM/s', default: 60, max: 65535, certainty: 'grounded', hint: '整数 RPM/s。' }),
            intField('vel', '速度', 2, { default: 600, max: 65535, certainty: 'grounded', hint: '0.1 RPM。' }),
            intField('clk', '行程', 4, { default: 1800, max: 0xffffffff, certainty: 'grounded', hint: '0.1° 行程幅值。' }),
            intField('motionMode', '运动模式', 1, { default: 2, max: 255, certainty: 'protocol', hint: '协议字段。' }),
            SYNC_FIELD(),
            CURRENT_LIMIT_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: 'CD 是 18 字节逻辑指令，会按 7 字节一组拆成 3 个扩展帧。',
      },
    ],
  },
  {
    id: 'read',
    name: '状态读取',
    open: false,
    items: READ_ITEMS.map((entry) => ({
      id: entry.key,
      name: entry.name,
      en: `read ${entry.en}`,
      sendNote: '读取请求',
      integrated: entry.certainty === 'grounded',
      readParam: entry,
      summary: `读取 ${entry.name}（readSysParams）。`,
      variants: [readVariant(entry)],
      note: `${entry.meaning} 另有 probeReadSysParams：逻辑指令完全相同，仅发送方式为单次。`,
    })),
  },
  {
    id: 'origin',
    name: '回零与原点',
    open: false,
    items: [
      {
        id: 'originSetZero',
        name: '原点置零',
        en: 'origin set zero 93',
        sendNote: '回零请求',
        integrated: false,
        summary: '把当前点设置为原点，可选是否保存。',
        variants: [
          V('base', '93', 0x93, [
            constant(0x88, '标识'),
            enumField('save', '保存', SAVE_OPTIONS, { default: 1, certainty: 'protocol', hint: '按固件 bool→uint8 透传，含义未在源码中定义。' }),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '网页未接入物理回零：只发送帧并给出模拟说明，不会移动位置。',
      },
      {
        id: 'originModifyParams',
        name: '原点参数配置',
        en: 'origin modify params 4C stall timeout',
        sendNote: '回零参数请求',
        integrated: false,
        summary: '写入回零相关参数（19 字节逻辑指令，拆成 3 个扩展帧）。',
        variants: [
          V('base', '4C', 0x4c, [
            constant(0xae, '标识'),
            enumField('save', '保存', SAVE_OPTIONS, { default: 1, certainty: 'protocol', hint: '按固件 bool→uint8 透传。' }),
            intField('mode', '回零模式', 1, { default: 0, max: 255, certainty: 'protocol', hint: '协议字段：固件未定义取值表。' }),
            intField('dir', '方向', 1, { default: 0, max: 255, certainty: 'protocol', hint: '协议字段：固件未定义取值表。' }),
            intField('vel', '回零速度', 2, { default: 0, max: 65535, certainty: 'protocol', hint: '参数名 vel；单位未在源码中定义。' }),
            intField('timeoutMs', '超时', 4, { unit: 'ms', default: 10000, max: 0xffffffff, certainty: 'name', hint: '参数名 timeoutMs（uint32 大端）。' }),
            intField('stallVel', '堵转速度阈值', 2, { default: 0, max: 65535, certainty: 'protocol', hint: '参数名 stallVel；单位未在源码中定义。' }),
            intField('stallMa', '堵转电流阈值', 2, { unit: 'mA', default: 0, max: 65535, certainty: 'name', hint: '参数名 stallMa（mA）。' }),
            intField('stallMs', '堵转判定时间', 2, { unit: 'ms', default: 0, max: 65535, certainty: 'name', hint: '参数名 stallMs（ms）。' }),
            enumField('powerOnTrigger', '上电触发', ON_OFF_OPTIONS, { default: 0, certainty: 'name', hint: '参数名 powerOnTrigger，按 0/1 透传。' }),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '网页未接入物理回零：只发送帧并给出模拟说明。',
      },
      {
        id: 'originTriggerReturn',
        name: '原点触发回零',
        en: 'origin trigger return 9A homing',
        sendNote: '回零触发请求',
        integrated: false,
        summary: '触发一次回零动作。',
        variants: [
          V('base', '9A', 0x9a, [
            intField('mode', '回零模式', 1, { default: 0, max: 255, certainty: 'protocol', hint: '协议字段：固件未定义取值表。' }),
            SYNC_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '演示不会执行真实的寻位动作，只在日志中给出模拟说明。',
      },
      {
        id: 'originInterrupt',
        name: '原点中断',
        en: 'origin interrupt 9C',
        sendNote: '回零中断请求',
        integrated: false,
        summary: '中断正在进行的回零动作。',
        variants: [
          V('base', '9C', 0x9c, [constant(0x48, '标识'), constant(PROTOCOL_CHECKSUM, '固定校验')]),
        ],
        note: '演示中只清空模拟的回零状态。',
      },
    ],
  },
  {
    id: 'config',
    name: '配置与维护',
    open: false,
    items: [
      {
        id: 'resetCurPosToZero',
        name: '当前位置清零',
        en: 'reset current position zero 0A',
        sendNote: '位置清零请求',
        integrated: true,
        summary: '把当前位置计数清零（不移动电机）。',
        variants: [
          V('base', '0A', 0x0a, [constant(0x6d, '标识'), constant(PROTOCOL_CHECKSUM, '固定校验')]),
        ],
        note: '模拟中直接把当前位置置 0。',
      },
      {
        id: 'resetClogProtection',
        name: '堵转保护复位',
        en: 'reset clog protection 0E stall',
        sendNote: '堵转复位请求',
        integrated: false,
        summary: '复位堵转保护状态。',
        variants: [
          V('base', '0E', 0x0e, [constant(0x52, '标识'), constant(PROTOCOL_CHECKSUM, '固定校验')]),
        ],
        note: '固件未暴露堵转标志的读取方式，演示只记录模拟说明。',
      },
      {
        id: 'modifyCtrlMode',
        name: '控制模式设置',
        en: 'modify control mode 46 ctrlMode',
        sendNote: '控制模式请求',
        integrated: false,
        summary: '写入控制模式，可选是否保存。',
        variants: [
          V('base', '46', 0x46, [
            constant(0x69, '标识'),
            enumField('save', '保存', SAVE_OPTIONS, { default: 0, certainty: 'protocol', hint: '按固件 bool→uint8 透传。' }),
            intField('ctrlMode', '控制模式', 1, { default: 0, max: 255, certainty: 'protocol', hint: '协议字段：固件未定义取值表，按 uint8 原样透传。' }),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '控制模式取值表未在源码中定义，界面不猜测每个取值的含义。',
      },
      {
        id: 'realtimePositionFeedback',
        name: '实时位置反馈间隔',
        en: 'realtime position feedback interval 11',
        sendNote: '反馈间隔请求',
        integrated: false,
        summary: '配置主动上报位置反馈的间隔。',
        variants: [
          V('base', '11', 0x11, [
            constant(0x18, '标识'),
            constant(0x36, '子功能'),
            intField('intervalMs', '反馈间隔', 2, { unit: 'ms', default: 100, max: 65535, certainty: 'name', hint: '参数名 intervalMs（uint16 大端）。0 表示关闭周期性上报。' }),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '演示中非 0 间隔会按 250 ms 下限产生模拟位置反馈帧。',
      },
    ],
  },
  {
    id: 'custom',
    name: '自定义帧',
    open: false,
    items: [
      {
        id: 'customFrame',
        name: '自定义帧',
        en: 'custom raw frame hex manual',
        sendNote: '自定义请求',
        integrated: false,
        custom: true,
        summary: '手工构造逻辑指令：功能码 + 参数字节 + 固定校验 0x6B。',
        variants: [V('base', '自定义', 0x00, [])],
        note: '未知功能码只发送 TX 帧：无解码器、不做模拟，也不会伪造成功结果。',
      },
    ],
  },
];

export const COMMAND_ITEMS = COMMAND_GROUPS.flatMap((group) =>
  group.items.map((item) => ({ ...item, groupId: group.id, groupName: group.name })),
);

const ITEM_BY_ID = new Map(COMMAND_ITEMS.map((item) => [item.id, item]));

export function getCommandItem(id) {
  return ITEM_BY_ID.get(id) ?? null;
}

export function getVariant(item, variantKey) {
  if (!item?.variants?.length) return null;
  return item.variants.find((variant) => variant.key === variantKey) ?? item.variants[0];
}

// Fields of a variant, custom commands excluded.
export function getFields(item, variantKey) {
  const variant = getVariant(item, variantKey);
  if (!variant) return [];
  return variant.layout.filter((segment) => segment.kind === 'field');
}

export function getField(item, variantKey, key) {
  return getFields(item, variantKey).find((field) => field.key === key) ?? null;
}

export function defaultValues(item, variantKey) {
  const values = {};
  for (const field of getFields(item, variantKey)) {
    values[field.key] = field.default;
  }
  return values;
}

export function opcodeLabel(item) {
  return item.variants.map((variant) => hexByte(variant.opcode)).join(' / ');
}

// ---------------------------------------------------------------------------
// Hex helpers
// ---------------------------------------------------------------------------

export function hexByte(value) {
  return value.toString(16).toUpperCase().padStart(2, '0');
}

export function formatBytes(bytes) {
  return bytes.map(hexByte).join(' ');
}

export function formatCanId(canId) {
  return `0x${canId.toString(16).toUpperCase().padStart(8, '0')}`;
}

export function canFrameId(address, packetIndex) {
  return ((address << 8) | packetIndex) >>> 0;
}

export function canAddressFromFrameId(frameId) {
  return (frameId >> 8) & 0xff;
}

export function canPacketIndexFromFrameId(frameId) {
  return frameId & 0xff;
}

// ---------------------------------------------------------------------------
// Validation helpers (integers only, no silent clamping anywhere)
// ---------------------------------------------------------------------------

export function validateAddress(value) {
  const text = String(value ?? '').trim();
  if (!text) return { ok: false, error: '请输入电机地址（1..255）' };
  if (!/^\d+$/.test(text)) return { ok: false, error: '电机地址必须是十进制整数' };
  const address = Number(text);
  if (!Number.isSafeInteger(address)) return { ok: false, error: '电机地址超出安全整数范围' };
  if (address < ADDRESS_MIN || address > ADDRESS_MAX) {
    return { ok: false, error: `电机地址需在 ${ADDRESS_MIN}..${ADDRESS_MAX} 之间` };
  }
  return { ok: true, value: address };
}

export function checkInteger(raw, field) {
  const label = field?.label ?? '字段';
  const min = field?.min ?? 0;
  const max = field?.max ?? 255;
  const text = String(raw ?? '').trim();
  if (!text) return { ok: false, error: `${label}不能为空` };
  const numeric = Number(text);
  if (!Number.isFinite(numeric)) {
    return { ok: false, error: `${label}必须是有限数字（不接受 NaN / Infinity）` };
  }
  if (!Number.isInteger(numeric)) {
    return { ok: false, error: `${label}必须是整数（原始字节字段不接受小数）` };
  }
  if (!Number.isSafeInteger(numeric)) {
    return { ok: false, error: `${label}超出可安全表示的范围` };
  }
  if (numeric < min || numeric > max) {
    return { ok: false, error: `${label}需在 ${min}..${max} 之间（不做静默截断）` };
  }
  return { ok: true, value: numeric };
}

export function checkEnum(raw, field) {
  const numeric = Number(raw);
  const option = field.options.find((entry) => entry.value === numeric);
  if (!option) return { ok: false, error: `${field.label}取值无效` };
  return { ok: true, value: option.value, label: option.label };
}

export function checkHexBytes(raw, { bytes = null, label = '字节' } = {}) {
  const text = String(raw ?? '').replace(/[\s,]+/g, '');
  if (!text) return { ok: true, value: [], empty: true };
  if (/[^0-9a-fA-F]/.test(text)) {
    return { ok: false, error: `${label}只能是十六进制字符（0-9 A-F）` };
  }
  if (text.length % 2 !== 0) {
    return { ok: false, error: `${label}每字节需要两位十六进制字符` };
  }
  const values = [];
  for (let i = 0; i < text.length; i += 2) {
    values.push(parseInt(text.slice(i, i + 2), 16));
  }
  if (bytes !== null && values.length !== bytes) {
    return { ok: false, error: `${label}需要 ${bytes} 字节，当前 ${values.length} 字节` };
  }
  return { ok: true, value: values };
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

function pushFieldBytes(out, outLabels, field, value) {
  const width = field.bytes;
  for (let index = 0; index < width; index += 1) {
    const shift = 8 * (width - 1 - index);
    const byte = (value >>> shift) & 0xff;
    out.push(byte);
    if (field.type === 'enum') {
      const option = field.options.find((entry) => entry.value === value);
      outLabels.push(field.byteLabel ?? (option ? option.label : field.label));
    } else if (width === 1) {
      outLabels.push(field.label);
    } else {
      const order = ['高字节', '次高字节', '次低字节', '低字节'];
      const names = width === 2 ? ['高字节', '低字节'] : order;
      outLabels.push(`${field.label}${names[index]}`);
    }
  }
}

/**
 * Encode one command variant into the full logical command bytes
 * (address + function code + parameters + fixed 0x6B checksum).
 */
export function encodeCommand({ item, variantKey, values, address }) {
  const variant = getVariant(item, variantKey);
  if (!variant) return { ok: false, errors: { _: '未找到指令变体' }, bytes: [] };
  if (item.custom) return encodeCustomCommand(values, address);

  const addressCheck = validateAddress(address);
  const errors = {};
  if (!addressCheck.ok) errors.address = addressCheck.error;

  const resolved = {};
  for (const segment of variant.layout) {
    if (segment.kind !== 'field') continue;
    const raw = values?.[segment.key];
    const check = segment.type === 'enum' ? checkEnum(raw, segment) : checkInteger(raw, segment);
    if (!check.ok) errors[segment.key] = check.error;
    else resolved[segment.key] = check.value;
  }
  if (Object.keys(errors).length > 0) return { ok: false, errors, bytes: [] };

  const bytes = [addressCheck.value];
  const labels = ['电机地址'];
  bytes.push(variant.opcode);
  labels.push('功能码');
  for (const segment of variant.layout) {
    if (segment.kind === 'const') {
      bytes.push(segment.value);
      labels.push(segment.label);
    } else {
      pushFieldBytes(bytes, labels, segment, resolved[segment.key]);
    }
  }
  return { ok: true, errors: {}, bytes, labels, variant };
}

/** Custom frame: function code + raw parameter bytes + fixed 0x6B. */
export function encodeCustomCommand(values, address) {
  const errors = {};
  const addressCheck = validateAddress(address);
  if (!addressCheck.ok) errors.address = addressCheck.error;

  const opcodeCheck = checkHexBytes(values?.opcode ?? '', { bytes: 1, label: '功能码' });
  if (!opcodeCheck.ok) errors.opcode = opcodeCheck.error;
  else if (opcodeCheck.empty) errors.opcode = '功能码不能为空（1 字节）';

  const paramsCheck = checkHexBytes(values?.params ?? '', { label: '参数字节' });
  if (!paramsCheck.ok) {
    errors.params = paramsCheck.error;
  } else {
    // address + function code + parameters + fixed checksum
    const total = paramsCheck.value.length + 3;
    if (total > LOGICAL_COMMAND_MAX_BYTES) {
      errors.params = `逻辑指令最多 ${LOGICAL_COMMAND_MAX_BYTES} 字节（当前 ${total} 字节）`;
    }
  }

  if (Object.keys(errors).length > 0) return { ok: false, errors, bytes: [] };

  const bytes = [addressCheck.value, opcodeCheck.value[0], ...paramsCheck.value, PROTOCOL_CHECKSUM];
  const labels = ['电机地址', '功能码'];
  paramsCheck.value.forEach((_, index) => labels.push(`参数 ${index + 1}`));
  labels.push('固定校验');
  return { ok: true, errors: {}, bytes, labels };
}

/**
 * Reverse of encodeCommand: recover field values from raw logical bytes.
 * Returns null when the length or any constant byte does not match. The
 * address byte is only checked for range, so hand-edited frames still resolve.
 */
export function decodeCommand(item, variant, bytes, address) {
  if (!variant || item.custom) return null;
  if (!Number.isInteger(bytes[0]) || bytes[0] < ADDRESS_MIN || bytes[0] > ADDRESS_MAX) return null;
  if (bytes[1] !== variant.opcode) return null;
  const values = {};
  let index = 2;
  for (const segment of variant.layout) {
    if (segment.kind === 'const') {
      if (bytes[index] !== segment.value) return null;
      index += 1;
      continue;
    }
    if (index + segment.bytes > bytes.length) return null;
    let value = 0;
    for (let offset = 0; offset < segment.bytes; offset += 1) {
      value = value * 256 + bytes[index + offset];
    }
    index += segment.bytes;
    values[segment.key] = value;
  }
  if (index !== bytes.length) return null;
  return { values };
}

/** Find the catalog command that produced exactly these bytes. */
export function identifyCommandBytes(bytes, address = null, preferredItemId = null) {
  if (!Array.isArray(bytes) || bytes.length < 3) return null;
  const candidates = [];
  for (const item of COMMAND_ITEMS) {
    if (item.custom) continue;
    for (const variant of item.variants) {
      const decoded = decodeCommand(item, variant, bytes, address);
      if (decoded) candidates.push({ item, variant, values: decoded.values });
    }
  }
  if (candidates.length === 0) return null;
  return candidates.find((entry) => entry.item.id === preferredItemId) ?? candidates[0];
}

// ---------------------------------------------------------------------------
// sendCommand() packet split (X42sProtocol::sendCommand)
//
//   payload   = logical command without the address and function code bytes
//   each frame carries the function code first, then up to 7 payload bytes
//   identifier = (address << 8) | packetIndex
// ---------------------------------------------------------------------------

export function buildFrames(bytes) {
  if (!Array.isArray(bytes) || bytes.length < 3) return [];
  const address = bytes[0];
  const opcode = bytes[1];
  const frames = [];
  let index = 2;
  let packetIndex = 0;
  while (index < bytes.length) {
    const chunk = bytes.slice(index, index + PAYLOAD_BYTES_PER_FRAME);
    const data = [opcode, ...chunk];
    const sourceIndexes = [1];
    for (let offset = 0; offset < chunk.length; offset += 1) sourceIndexes.push(index + offset);
    frames.push({
      packetIndex,
      canId: canFrameId(address, packetIndex),
      dlc: data.length,
      data,
      sourceIndexes,
      extended: true,
      remote: false,
    });
    index += PAYLOAD_BYTES_PER_FRAME;
    packetIndex += 1;
  }
  return frames;
}

export function frameAnnotation(bytes, labels, frame) {
  return frame.data
    .map((byte, index) => `${hexByte(byte)} ${labels[frame.sourceIndexes[index]] ?? '参数'}`)
    .join(' · ');
}

// ---------------------------------------------------------------------------
// Raw logical hex parsing / validation
// ---------------------------------------------------------------------------

export function parseLogicalHex(text) {
  const errors = [];
  const cleaned = String(text ?? '').replace(/[\s,]+/g, '');
  if (!cleaned) return { ok: false, errors: ['请输入逻辑指令字节'], bytes: [] };
  if (/[^0-9a-fA-F]/.test(cleaned)) {
    return { ok: false, errors: ['只允许十六进制字符（0-9 A-F），可用空格分隔'], bytes: [] };
  }
  if (cleaned.length % 2 !== 0) {
    return { ok: false, errors: ['每个字节需要两位十六进制字符（当前字符数为奇数）'], bytes: [] };
  }
  const bytes = [];
  for (let i = 0; i < cleaned.length; i += 2) {
    bytes.push(parseInt(cleaned.slice(i, i + 2), 16));
  }
  errors.push(...validateLogicalBytes(bytes));
  return { ok: errors.length === 0, errors, bytes };
}

export function validateLogicalBytes(bytes) {
  const errors = [];
  if (bytes.length < 3) {
    errors.push('逻辑指令至少需要 3 字节：地址 + 功能码 + 固定校验 6B');
    return errors;
  }
  if (bytes.length > LOGICAL_COMMAND_MAX_BYTES) {
    errors.push(`逻辑指令超过固件 uint8 长度上限（${LOGICAL_COMMAND_MAX_BYTES} 字节）`);
  }
  const address = bytes[0];
  if (address < ADDRESS_MIN || address > ADDRESS_MAX) {
    errors.push(`首字节是电机地址，需在 ${ADDRESS_MIN}..${ADDRESS_MAX} 之间（当前 ${address}）`);
  }
  if (bytes[bytes.length - 1] !== PROTOCOL_CHECKSUM) {
    errors.push(`缺少固定校验字节：最后一位必须是 6B（当前 ${hexByte(bytes[bytes.length - 1])}）`);
  }
  return errors;
}

// ---------------------------------------------------------------------------
// Catalog search
// ---------------------------------------------------------------------------

function searchCorpus(item) {
  const fields = item.variants.flatMap((variant) =>
    variant.layout
      .filter((segment) => segment.kind === 'field')
      .map((field) => `${field.key} ${field.label} ${field.unit ?? ''}`),
  );
  return [
    item.id,
    item.name,
    item.en ?? '',
    item.groupName,
    item.summary,
    opcodeLabel(item),
    ...item.variants.map((variant) => hexByte(variant.opcode)),
    ...fields,
  ]
    .join(' ')
    .toLowerCase();
}

const SEARCH_INDEX = new Map(COMMAND_ITEMS.map((item) => [item.id, searchCorpus(item)]));

export function searchCommands(query) {
  const text = String(query ?? '').trim().toLowerCase().replace(/^0x/, '');
  if (!text) return COMMAND_ITEMS;
  const needle = text.replace(/\s+/g, '');
  return COMMAND_ITEMS.filter((item) => {
    const corpus = SEARCH_INDEX.get(item.id) ?? '';
    return corpus.includes(text) || corpus.replace(/\s+/g, '').includes(needle);
  });
}

export function groupItems(groupId, query) {
  const matches = new Set(searchCommands(query).map((item) => item.id));
  const group = COMMAND_GROUPS.find((entry) => entry.id === groupId);
  if (!group) return [];
  return group.items.filter((item) => matches.has(item.id));
}
