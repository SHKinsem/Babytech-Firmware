// Pure, framework-free X42S / X28S protocol model.
//
// Sources of truth (see references/):
//   X42sProtocol.cpp / .h  byte layout of every command and the sendCommand()
//                          packet split.
//   x42s_can_id.h          address in bits 8..15, packet index in bits 0..7.
//   MotionCore.h           wire units (0.1 degree, 0.1 RPM, mA, whole RPM/s),
//                          ack status bytes and the trapezoid duration math.
//   ZDT_X42S 手册 V1.0.5   field names, units and enum tables for the X
//                          firmware (pages cited per hint). The manual is a
//                          second source, never a replacement for the copied
//                          firmware layout: byte layouts stay identical.
//
// Firmware baseline for this tool is X firmware (no Emm encoder variant), and
// the manual is only ever read as a statement about X firmware defaults; the
// prototype never auto-detects the firmware at runtime.
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
//
// maxSpeedRpm stays at the local 120 RPM ceiling. The manual states the X
// speed field range as 0-7E30 for "0-3000.0 RPM", but 30000 is 0x7530, so the
// manual contradicts itself (see the manual's own 原文不一致 list). That
// conflict is documentation-only and must not raise this local bound.
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

// How well a value is established by the copied firmware sources or the manual.
export const CERTAINTY = {
  manual: '手册明确',
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

// Manual V1.0.5: dir=00/01 is CW/CCW (p51-p55), sync=00/01 is immediate /
// cache-until-triggered (p40, p51), save=00/01 is do-not-store / store (p76).
const DIRECTION_OPTIONS = [
  { value: 0, label: 'CW' },
  { value: 1, label: 'CCW' },
];

const DIRECTION_HINT = '方向 00/01 分别表示 CW（顺时针）/ CCW（逆时针）——V1.0.5 p51-p55 的取值表与固件 MotionCore 用的同一字节一致。本工具统一按 CW/CCW 显示，不再自造方向说法。';

const SAVE_OPTIONS = [
  { value: 1, label: '保存' },
  { value: 0, label: '不保存' },
];

const SAVE_HINT = '是否存储 00/01 分别表示不存储 / 存储，存储后掉电不丢失参数（V1.0.5 p76、p61）。';

// Manual p61-p64: homing mode table, used verbatim, no extra values invented.
const HOME_MODE_OPTIONS = [
  { value: 0, label: '单圈就近' },
  { value: 1, label: '单圈方向' },
  { value: 2, label: '无限位碰撞' },
  { value: 3, label: '限位' },
  { value: 4, label: '绝对零点' },
  { value: 5, label: '上次掉电位置' },
];

const HOME_MODE_HINT = '手册 V1.0.5 p61-p64：回零模式 00 单圈就近 / 01 单圈方向 / 02 无限位碰撞 / 03 限位 / 04 回到绝对位置坐标零点 / 05 回到上次掉电位置角度，默认 00。接线与掉电记录条件见手册“3.2 原点回零”。';

// Manual p54-p55 (FB/CB) and p55 (FD/CD): the position payload is an angle in
// 0.1 degree counts for the X firmware, not an encoder pulse count.
const MOTION_MODE_OPTIONS = [
  { value: 0, label: '相对上一目标' },
  { value: 1, label: '绝对零点' },
  { value: 2, label: '相对当前位置' },
];

const MOTION_MODE_HINT = '手册 V1.0.5 p54-p55：运动模式 00/01/02 分别表示相对上一输入目标位置 / 相对坐标零点进行绝对位置运动 / 相对当前实时位置进行相对位置运动。本工具默认 02，并且只在 02 时做模拟运动，其余模式只发帧。';

const DIRECTION_FIELD = () =>
  enumField('dir', '方向', DIRECTION_OPTIONS, { default: 0, certainty: 'manual', hint: DIRECTION_HINT });

const MOTION_MODE_FIELD = () =>
  enumField('motionMode', '运动模式', MOTION_MODE_OPTIONS, { default: 2, certainty: 'manual', hint: MOTION_MODE_HINT });

const SYNC_FIELD = () =>
  enumField('sync', '同步执行', [
    { value: 0, label: '立即执行' },
    { value: 1, label: '缓存待同步' },
  ], {
    default: 0,
    byteLabel: '同步',
    certainty: 'manual',
    hint: '同步标志 00/01 分别表示立即执行 / 先缓存当前命令（V1.0.5 p40、p51-p55），缓存后由广播 FF 触发。本工具仍按模拟队列处理，未触发前不改变电机的模拟读数。',
  });

const CURRENT_LIMIT_FIELD = (key = 'maxCurrentMa') =>
  intField(key, '电流上限', 2, {
    unit: 'mA',
    max: CURRENT_MA_MAX,
    default: 1000,
    certainty: 'grounded',
    hint: 'mA。固件 constrainCurrentMa 会把越界值截断到 0..5000，本工具改为直接报错，不做静默截断；手册 V1.0.5 的最大电流范围同为 0000-1388，即 0-5000 mA。',
  });

// ---------------------------------------------------------------------------
// Command variants
//
// A "variant" is one real firmware function code (for example F5 and C5 are the
// plain and current-limited torque commands). Items that have two variants are
// shown once in the library with both codes, exactly like the reference design.
// ---------------------------------------------------------------------------

const V = (key, label, opcode, layout, extra = {}) => ({ key, label, opcode, layout, ...extra });

// Most reads are three-byte logical requests: [addr][opcode][0x6B]. The reply
// layouts and units below come from the manual V1.0.5 "5.5 读取系统参数命令"
// section (p67-p75) for the X firmware. Bulk 0x42/0x43 replies are documented
// separately in section 5.8; the device trace reassembles complete X-firmware
// replies while incomplete or other-firmware replies stay raw.
//
// `simulated` marks the four opcodes src/simulation.js actually has a reply
// layout for (0x27 / 0x35 / 0x36 / 0x3A). Everything else only sends TX.
const READ_ITEMS = [
  {
    key: 'readOptions',
    opcode: 0x1a,
    name: '选项参数 (raw)',
    en: 'Options option bits raw',
    certainty: 'manual',
    meaning: '0x1A 读取选项参数状态（手册 V1.0.5 p77）：bit0 电机类型（0=1.8°/1=0.9°）、bit1 固件类型（0=X 固件 / 1=Emm 固件）、bit2 控制模式（0 开环/1 闭环）、bit4 运动正方向（0=CW/1=CCW）、bit5 按键锁定、bit7 缩小 10 倍输入、bit8-bit9 参数锁定等级。手册文字描述了 bit8-bit9，但位表只列到 bit7，返回总宽度无法确定，因此本工具只发帧、给出原始字节，不推断长度与类型。',
  },
  {
    key: 'readVer',
    opcode: 0x1f,
    name: '版本 Ver',
    en: 'Ver firmware hardware version',
    certainty: 'manual',
    meaning: '0x1F 版本帧：[1F][固件版本 u16][硬件版本 u16][0x6B]（手册 V1.0.5 p67）。固件版本原始值例：200 = V2.0.0。手册把硬件版本 16 位拆成 HW_Series(bit15-12)/HW_Type(bit11-8)/HW_Ver(bit7-0)，但只给了位表、没有独立字节图，故本工具只输出原始 16 位，不拆分。',
  },
  {
    key: 'readRl',
    opcode: 0x20,
    name: '相电阻/相电感 (mΩ/uH)',
    en: 'Rl phase resistance inductance',
    certainty: 'manual',
    meaning: '0x20 相电阻/相电感帧：[20][相电阻 u16][相电感 u16][0x6B]；单位 mΩ / uH（手册 V1.0.5 p67）。',
  },
  { key: 'readPid', opcode: 0x21, name: 'PID 参数', en: 'Pid', certainty: 'protocol', meaning: '固件枚举名 Pid；手册 V1.0.5 的“读取系统参数命令”章节未列出该功能码，含义与应答布局都没有依据。' },
  {
    key: 'readVbus',
    opcode: 0x24,
    name: '母线电压 Vbus (mV)',
    en: 'Vbus bus voltage mV',
    certainty: 'manual',
    meaning: '0x24 总线电压帧：[24][VBus u16][0x6B]；单位 mV，即供电 V+ 经过反接二极管后的电压（手册 V1.0.5 p68）。',
  },
  {
    key: 'readCbus',
    opcode: 0x26,
    name: '总线电流 Cbus (mA)',
    en: 'Cbus bus current mA',
    certainty: 'manual',
    meaning: '0x26 总线电流帧：[26][CBus u16][0x6B]；单位 mA，即 V+ 引脚供电电流（手册 V1.0.5 p68）。手册注明该值由相电流和相电压换算得到，可能存在偏差，仅提供趋势参考。',
  },
  {
    key: 'readCpha',
    opcode: 0x27,
    name: '相电流 (mA)',
    en: 'Cpha current mA',
    certainty: 'manual',
    simulated: true,
    meaning: '0x27 相电流帧：[27][mA15..8][mA7..0][0x6B]；单位 mA，电机实际工作电流（手册 V1.0.5 p69；MotionCore.h 同一布局）。',
  },
  {
    key: 'readEncl',
    opcode: 0x31,
    name: '线性化编码器 (0..65535)',
    en: 'Encl linearized encoder',
    certainty: 'manual',
    meaning: '0x31 线性化编码器帧：[31][0-65535][0x6B]；0-65535 表示 0-360°（手册 V1.0.5 p69）。线性化编码器是单圈绝对值，到 65535 后重新从 0 开始。',
  },
  {
    key: 'readPulses',
    opcode: 0x32,
    name: '输入脉冲数',
    en: 'Pulses input pulse count',
    certainty: 'manual',
    meaning: '0x32 输入脉冲数帧：[32][符号][脉冲数 u32][0x6B]；符号 00/01 分别表示正/负（手册 V1.0.5 p70）。手册注明默认 16 细分下 3200 个脉冲表示一圈 360°，细分改变后需重新换算。',
  },
  {
    key: 'readTpos',
    opcode: 0x33,
    name: '目标位置 Tpos (0.1°)',
    en: 'Tpos target position',
    certainty: 'manual',
    meaning: '0x33 电机目标位置帧：[33][符号][目标位置 u32][0x6B]，符号 00/01 分别表示正/负（手册 V1.0.5 p70）。X 固件角度 = 值 / 10，例如返回 16 即 1.6°；Emm 固件为 65536 计数一圈。本工具按 X 固件的 0.1°/计数显示。',
  },
  {
    key: 'readSetTarget',
    opcode: 0x34,
    name: '设定目标位置 (0.1°)',
    en: 'SetTarget realtime target position',
    certainty: 'manual',
    meaning: '0x34 电机实时设定的目标位置帧：[34][符号][设定目标位置 u32][0x6B]（手册 V1.0.5 p71）。符号 00/01 表示开环模式实时位置的正/负；X 固件角度 = 值 / 10。',
  },
  {
    key: 'readVel',
    opcode: 0x35,
    name: '速度 (0.1 RPM)',
    en: 'Vel velocity RPM',
    certainty: 'manual',
    simulated: true,
    meaning: '0x35 实时转速帧：[35][符号][转速 u16][0x6B]，范围 0000-1388，符号 00/01 分别表示正/负（手册 V1.0.5 p71）。X 固件单位 0.1 RPM（返回值需 /10），Emm 固件单位 RPM；本工具按 X 固件显示。',
  },
  {
    key: 'readCpos',
    opcode: 0x36,
    name: '当前位置 (0.1°)',
    en: 'Cpos current position',
    certainty: 'manual',
    simulated: true,
    meaning: '0x36 电机实时位置帧：[36][符号][实时位置 u32][0x6B]，符号 00/01 分别表示正/负（手册 V1.0.5 p72）。X 固件返回角度 = 值 / 10，Emm 固件为 65536 计数一圈。本工具按 X 固件的 0.1°/计数显示；输入位置缩放配置不作为反馈换算依据。',
  },
  {
    key: 'readPerr',
    opcode: 0x37,
    name: '位置误差 (0.01°)',
    en: 'Perr position error',
    certainty: 'manual',
    meaning: '0x37 位置误差帧：[37][符号][误差 u32][0x6B]，符号 00/01 分别表示正/负（手册 V1.0.5 p73）。X 固件角度 = 值 / 100，例如返回 8 即 0.08°：位置误差用 0.01°/计数，比位置读取的 0.1°/计数细一档。',
  },
  {
    key: 'readTemp',
    opcode: 0x39,
    name: '驱动温度 (℃)',
    en: 'Temp driver temperature',
    certainty: 'manual',
    meaning: '0x39 驱动温度帧：[39][温度符号][温度][0x6B]；单位 ℃（手册 V1.0.5 p72）。温度符号 00/01 分别表示负数/正数，与其它带符号读取的 00=正 / 01=负相反。',
  },
  {
    key: 'readFlag',
    opcode: 0x3a,
    name: '电机状态标志 (3A)',
    en: 'Flag motor status bits',
    certainty: 'manual',
    simulated: true,
    meaning: '0x3A 电机状态标志帧：[3A][标志][0x6B]（手册 V1.0.5 p73-p74）：bit0 使能、bit1 位置到达、bit2 堵转、bit3 堵转保护、bit4 左限位输入引脚电平、bit5 右限位输入引脚电平、bit7 掉电标志（默认 0，掉电重启恢复 0）。限位位只表示输入引脚高低电平，不代表限位已触发。',
  },
  {
    key: 'readOrg',
    opcode: 0x3b,
    name: '回零状态标志 (3B)',
    en: 'Org homing status bits',
    certainty: 'manual',
    meaning: '0x3B 回零状态标志帧：[3B][标志][0x6B]（手册 V1.0.5 p62-p63）：bit0 编码器就绪、bit1 校准表就绪、bit2 正在回零、bit3 回零失败、bit4 过热保护、bit5 过流保护、bit6/bit7 保留。status & 0x0C 为 04 表示正在回零、08 表示回零失败、00 表示未在回零且未报失败——默认也是 00，不能据此确认本次回零已经完成。',
  },
  {
    key: 'readFlags',
    opcode: 0x3c,
    name: '回零 + 电机状态 (3C)',
    en: 'Flags homing and motor status',
    certainty: 'manual',
    meaning: '0x3C 帧：[3C][回零状态标志][电机状态标志][0x6B]，两个标志字节分别见 0x3B 与 0x3A 的说明（手册 V1.0.5 p74）。',
  },
  {
    key: 'readIo',
    opcode: 0x3d,
    name: '引脚 IO 电平 (3D)',
    en: 'Io pin level status',
    certainty: 'manual',
    meaning: '0x3D 引脚 IO 电平帧：[3D][电平状态][0x6B]（手册 V1.0.5 p75）：bit0 使能引脚电平、bit2 脉冲引脚电平、bit4 方向引脚电平（0 低电平 / 1 高电平）、bit5 方向引脚模式（0 输入 / 1 输出）、bit1 与 bit3 恒为 0、bit6/bit7 保留。',
  },
  { key: 'readConf', opcodes: [0x42, 0x6c], name: '配置读取 Conf', en: 'Conf config', certainty: 'manual', meaning: '读取所有驱动参数：[addr][42][6C][6B]。手册 V1.0.5 第 5.8 节；完整五包 X 固件读回会显示主要配置值，其余字段和缺包记录保留原始帧；页面按 X 固件解释，不自动识别 Emm。' },
  { key: 'readState', opcodes: [0x43, 0x7a], name: '状态读取 State', en: 'State status', certainty: 'manual', meaning: '读取系统状态参数：[addr][43][7A][6B]。手册 V1.0.5 第 5.8 节；完整五包 X 固件读回会显示主要状态值，缺包记录保留原始帧；页面按 X 固件解释，不自动识别 Emm。' },
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
            ], { default: 1, certainty: 'manual', hint: 'V1.0.5 p50：使能/关闭使能 [F3][AB][使能][同步][6B]；关闭使能会松开电机轴。源码 enableControl(addr, state, sync) 按 0/1 透传，两侧一致。' }),
            SYNC_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '同步选项：手册明确 01 表示先缓存当前命令，由广播 FF 触发；网页侧为模拟排队。',
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
        note: '手册 V1.0.5 p60：立即停止为 [FE][98][同步][6B]；广播触发已缓存运动为 [FF][66][6B]。模拟中停止会同时作废未完成的运动与待触发队列，不会再有迟到的完成记录。',
      },
      {
        id: 'syncTrigger',
        name: '同步触发',
        en: 'synchronous motion trigger FF',
        sendNote: '同步触发',
        integrated: true,
        summary: '执行此前以“缓存待同步”方式提交的指令。',
        variants: [
          V('base', 'FF', 0xff, [constant(0x66, '标识'), constant(PROTOCOL_CHECKSUM, '固定校验')]),
        ],
        note: '手册 V1.0.5 p60：广播发送 00 FF 66 6B 只执行已缓存的命令。网页侧只执行模拟队列；无队列时仅发送帧。',
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
            DIRECTION_FIELD(),
            intField('accel', '电流加速度', 2, { unit: 'mA/s', default: 1000, max: 65535, certainty: 'manual', hint: 'V1.0.5 p51：斜率（加速度）范围 0000-FFFF，即 0-65535 mA/S（毫安每秒）。' }),
            CURRENT_LIMIT_FIELD('currentMa'),
            SYNC_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
          V('limit', 'C5 限速', 0xc5, [
            DIRECTION_FIELD(),
            intField('accel', '电流加速度', 2, { unit: 'mA/s', default: 1000, max: 65535, certainty: 'manual', hint: 'V1.0.5 p51：斜率范围 0000-FFFF，即 0-65535 mA/S。' }),
            CURRENT_LIMIT_FIELD('currentMa'),
            SYNC_FIELD(),
            intField('maxSpeed', '限速', 2, { unit: '0.1 RPM', default: 0, max: 65535, certainty: 'manual', hint: 'V1.0.5 p51：C5 的“最大速度”范围 0000-7E30，保留一位小数，即 0-3000.0 RPM，按原始 16 位大端下发。手册同时把 30000 写成 0x7E30（30000 应为 0x7530），该矛盾只影响范围描述，不改变本工具的 0.1 RPM 换算与本地试动上限。' }),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '力矩指令按电流（mA）下发，不是扭矩 Nm：源码只暴露电流值，手册 V1.0.5 p51 的电流范围同为 0000-1388（0-5000 mA）。',
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
            DIRECTION_FIELD(),
            intField('acc', '加速度', 2, { unit: 'RPM/s', default: 0, max: 65535, certainty: 'manual', hint: 'V1.0.5 p52（X 固件）：加速度范围 0000-FFFF，即 0-65535 RPM/S（转每分钟每秒），不分档位、无 ×10 缩放。' }),
            intField('vel', '速度', 2, { unit: '0.1 RPM', default: 600, max: 65535, certainty: 'manual', hint: 'V1.0.5 p52（X 固件）：速度保留一位小数，即 0.1 RPM/计数。600 = 60.0 RPM。' }),
            SYNC_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
          V('limit', 'C6 限流', 0xc6, [
            DIRECTION_FIELD(),
            intField('acc', '加速度', 2, { unit: 'RPM/s', default: 0, max: 65535, certainty: 'manual', hint: 'V1.0.5 p52（X 固件）：加速度 0000-FFFF，即 0-65535 RPM/S。' }),
            intField('vel', '速度', 2, { unit: '0.1 RPM', default: 600, max: 65535, certainty: 'manual', hint: 'V1.0.5 p52（X 固件）：速度 0.1 RPM/计数。' }),
            SYNC_FIELD(),
            CURRENT_LIMIT_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '速度模式为连续运动：演示中最多模拟 10 秒，随后自动停转（模拟限制）。手册 V1.0.5 把 X 固件的速度范围写成 0000-7E30 = 0-3000.0 RPM，但 30000 应为 0x7530，该处原文自相矛盾，本工具不据此改动校验范围。',
      },
      {
        id: 'passthroughPosition',
        name: '直通位置',
        en: 'passthrough position angle FB CB',
        sendNote: '直通位置请求',
        integrated: true,
        summary: '直通位置控制，速度与行程由驱动器内部规划。',
        variants: [
          V('base', 'FB 基础', 0xfb, [
            DIRECTION_FIELD(),
            intField('vel', '速度', 2, { unit: '0.1 RPM', default: 600, max: 65535, certainty: 'manual', hint: 'V1.0.5 p54（X 固件）：速度(0.1RPM)，保留一位小数。600 = 60.0 RPM。' }),
            intField('clk', '位置角度', 4, { unit: '0.1°', default: 1800, max: 0xffffffff, certainty: 'manual', hint: 'V1.0.5 p54（X 固件）：位置角度 00000000-FFFFFFFF，单位为 0.1°，值为 1 表示 0.1°、10 表示 1°。X 固件按 0.1°/计数，不是 Emm 固件的脉冲数；位置缩放开到 0.01° 时需另算，本工具按默认 0.1° 处理。' }),
            MOTION_MODE_FIELD(),
            SYNC_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
          V('limit', 'CB 限流', 0xcb, [
            DIRECTION_FIELD(),
            intField('vel', '速度', 2, { unit: '0.1 RPM', default: 600, max: 65535, certainty: 'manual', hint: 'V1.0.5 p54（X 固件）：速度(0.1RPM)。' }),
            intField('clk', '位置角度', 4, { unit: '0.1°', default: 1800, max: 0xffffffff, certainty: 'manual', hint: 'V1.0.5 p54：位置角度(0.1°)，X 固件按 0.1°/计数。' }),
            MOTION_MODE_FIELD(),
            SYNC_FIELD(),
            CURRENT_LIMIT_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '手册 V1.0.5 p54 明确直通位置的行程就是 0.1° 角度；演示仍只按该 0.1° 约定折算可视化，不把角度当成脉冲数。',
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
            DIRECTION_FIELD(),
            intField('accel', '加速度', 2, { unit: 'RPM/s', default: 60, max: 65535, certainty: 'manual', hint: 'V1.0.5 p55（X 固件）：加速加速度 0000-FFFF，单位 RPM/S，整数、无 ×10 缩放。' }),
            intField('decel', '减速度', 2, { unit: 'RPM/s', default: 60, max: 65535, certainty: 'manual', hint: 'V1.0.5 p55（X 固件）：减速加速度 0000-FFFF，单位 RPM/S。' }),
            intField('vel', '速度', 2, { unit: '0.1 RPM', default: 600, max: 65535, certainty: 'manual', hint: 'V1.0.5 p55（X 固件）：最大速度(0.1RPM)。600 = 60.0 RPM。' }),
            intField('clk', '位置角度', 4, { unit: '0.1°', default: 1800, max: 0xffffffff, certainty: 'manual', hint: 'V1.0.5 p55（X 固件）：位置角度(0.1°)，幅值由 dir 决定方向。1800 = 180.0°。' }),
            MOTION_MODE_FIELD(),
            SYNC_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
          V('limit', 'CD 限流', 0xcd, [
            DIRECTION_FIELD(),
            intField('accel', '加速度', 2, { unit: 'RPM/s', default: 60, max: 65535, certainty: 'manual', hint: 'V1.0.5 p55：加速加速度，整数 RPM/S。' }),
            intField('decel', '减速度', 2, { unit: 'RPM/s', default: 60, max: 65535, certainty: 'manual', hint: 'V1.0.5 p55：减速加速度，整数 RPM/S。' }),
            intField('vel', '速度', 2, { unit: '0.1 RPM', default: 600, max: 65535, certainty: 'manual', hint: 'V1.0.5 p55：最大速度(0.1RPM)。' }),
            intField('clk', '位置角度', 4, { unit: '0.1°', default: 1800, max: 0xffffffff, certainty: 'manual', hint: 'V1.0.5 p55：位置角度(0.1°)。' }),
            MOTION_MODE_FIELD(),
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
      // "integrated" means the local simulation can build a reply frame for it,
      // which is independent of how well the manual documents the field.
      integrated: entry.simulated === true,
      readParam: entry,
      summary: `读取 ${entry.name}。${['readOptions','readPid'].includes(entry.key) ? '返回布局未确认，保留原始帧。' : ['readConf','readState'].includes(entry.key) ? '完整五包 X 固件读回会展示主要参数，缺包保留原始帧。' : '实机有效回包在发送区和右侧查询结果中解释。'}`,
      variants: [readVariant(entry)],
      note: entry.meaning,
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
            enumField('save', '保存', SAVE_OPTIONS, { default: 1, certainty: 'manual', hint: SAVE_HINT }),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '手册 V1.0.5 p61：设置单圈零点为 [93][88][是否存储][6B]。网页未接入物理回零：只发送帧并给出模拟说明，不会移动位置。',
      },
      {
        id: 'originModifyParams',
        name: '原点参数配置',
        en: 'origin modify params 4C stall timeout homing',
        sendNote: '回零参数请求',
        integrated: false,
        summary: '写入回零相关参数（20 字节逻辑指令，含固定校验 6B，拆成 3 个扩展帧）。',
        variants: [
          V('base', '4C', 0x4c, [
            constant(0xae, '标识'),
            enumField('save', '保存', SAVE_OPTIONS, { default: 1, certainty: 'manual', hint: SAVE_HINT }),
            enumField('mode', '回零模式', HOME_MODE_OPTIONS, { default: 0, certainty: 'manual', hint: HOME_MODE_HINT }),
            enumField('dir', '回零方向', DIRECTION_OPTIONS, { default: 0, certainty: 'manual', hint: 'V1.0.5 p64：回零方向 00/01 分别表示 CW/CCW，默认为 CW。' }),
            intField('vel', '回零速度', 2, { unit: 'RPM', default: 30, max: 3000, certainty: 'manual', hint: 'V1.0.5 p64-p65：回零速度范围 0000-0BB8，即 0-3000 RPM，是整数 RPM、没有 0.1 RPM 的 ×10 缩放，默认 30 RPM。' }),
            intField('timeoutMs', '回零超时', 4, { unit: 'ms', default: 10000, max: 0xffffffff, certainty: 'manual', hint: 'V1.0.5 p65：回零超时时间 uint32 大端，单位毫秒，默认 10000 ms；超时会自动退出回零。' }),
            intField('stallVel', '碰撞检测转速', 2, { unit: 'RPM', default: 300, max: 3000, certainty: 'manual', hint: 'V1.0.5 p64-p65：碰撞回零检测转速范围 0000-0BB8，即 0-3000 整数 RPM（无 ×10 缩放），默认 300 RPM。检测条件为实际转速低于该阈值。' }),
            intField('stallMa', '碰撞检测电流阈值', 2, { unit: 'mA', default: 800, max: CURRENT_MA_MAX, certainty: 'manual', hint: 'V1.0.5 p64-p65：回零碰撞的检测阈值（不是限流值），范围 0000-1388，即 0-5000 mA，默认 800 mA；检测条件为实际相电流高于该阈值，且转速低于碰撞检测转速并持续超过碰撞检测时间。阈值高于当前生效的闭环最大相电流（见 0x45）时，实际电流永远达不到该阈值，碰撞回零可能一直检测不到。' }),
            intField('stallMs', '碰撞检测时间', 2, { unit: 'ms', default: 60, max: 65535, certainty: 'manual', hint: 'V1.0.5 p64-p65：碰撞回零检测时间范围 0000-FFFF，单位 ms，默认 60 ms。三个条件需同时满足且持续时间超过该阈值。' }),
            enumField('powerOnTrigger', '上电自动回零', [
              { value: 1, label: '使能' },
              { value: 0, label: '不使能' },
            ], { default: 0, certainty: 'manual', hint: 'V1.0.5 p65：O_POT_En 是否使能上电自动触发回零，00/01 分别表示不使能/使能。' }),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '手册 V1.0.5 p64-p65 给出全部字段的范围与默认值（回零速度 30 RPM、超时 10000 ms、碰撞检测 300 RPM / 800 mA / 60 ms、上电自动回零不使能），本条目默认值照此对齐。网页未接入物理回零：只发送帧并给出模拟说明。',
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
            enumField('mode', '回零模式', HOME_MODE_OPTIONS, { default: 0, certainty: 'manual', hint: HOME_MODE_HINT }),
            SYNC_FIELD(),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '手册 V1.0.5 p61-p62：触发回零为 [9A][回零模式][同步标志][6B]，例如 01 9A 02 00 6B 触发无限位碰撞回零；回零失败标志与正在回零标志见 0x3B。演示不会执行真实的寻位动作，只在日志中给出模拟说明。',
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
        note: '手册 V1.0.5 p62：强制中断并退出回零为 [9C][48][6B]，回零过程中可中断。演示中只清空模拟的回零状态，不伪造回零完成。',
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
        note: '手册 V1.0.5 p48：将当前位置角度清零为 [0A][6D][6B]，不移动电机。模拟中直接把当前位置置 0。',
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
        note: '手册 V1.0.5 p48：解除堵转/过热/过流保护为 [0E][52][6B]。堵转与过流/过热标志可以读（0x3A bit2/bit3、0x3B bit4/bit5），但固件未暴露本机保护状态的仿真，演示只记录模拟说明。',
      },
      {
        id: 'modifyCtrlMode',
        name: '控制模式设置',
        en: 'modify control mode 46 ctrlMode open loop closed loop',
        sendNote: '控制模式请求',
        integrated: false,
        summary: '写入控制模式，可选是否保存。',
        variants: [
          V('base', '46', 0x46, [
            constant(0x69, '标识'),
            enumField('save', '保存', SAVE_OPTIONS, { default: 0, certainty: 'manual', hint: SAVE_HINT }),
            enumField('ctrlMode', '控制模式', [
              { value: 0, label: '开环' },
              { value: 1, label: '闭环' },
            ], { default: 1, certainty: 'manual', hint: 'V1.0.5 p79：控制模式 00/01 分别表示开环/闭环控制模式，默认值为 01。该字段与 0x1A 选项参数的 bit2 同义。' }),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '手册 V1.0.5 p79 确认 [46][69][是否存储][控制模式][6B] 与固件 0x46/0x69 一致；手册建议尽量在电机停止状态下修改。默认值改为手册的 01（闭环）。',
      },
      {
        id: 'closedLoopCurrentLimit',
        name: '闭环最大相电流',
        en: 'closed loop max phase current limit 45',
        sendNote: '电流上限请求',
        integrated: false,
        summary: '写入闭环模式的最大相电流（全局生效，可选是否保存）。',
        variants: [
          V('base', '45', 0x45, [
            constant(0x66, '辅助码'),
            enumField('save', '保存', SAVE_OPTIONS, { default: 0, certainty: 'manual', hint: SAVE_HINT }),
            intField('currentMa', '闭环最大相电流', 2, { unit: 'mA', default: 120, max: CURRENT_MA_MAX, certainty: 'manual', hint: 'V1.0.5 p82（5.6.13）：[45][66][是否存储][闭环模式最大电流 u16 大端][6B]，范围 0000-1388，即 0-5000 mA；X 固件出厂默认 3000 mA。这里的 120 mA 只是编辑示例，不是从驱动器读回的实际值——本工具没有该参数的读取命令，也不会把示例当成已生效的配置。' }),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '手册 V1.0.5 p82（X 固件）：该值是全局闭环最大相电流——「电机正常运行过程中，不管什么状态都不会超过此值」，不是 4C 回零的碰撞检测阈值，也不只作用于回零；它也不是实时电流读数（读数见 0x27）。它与碰撞检测阈值（默认 800 mA）相互独立：把上限压到接近或低于该阈值时，实际电流达不到阈值，碰撞回零可能永远检测不到。参数写入前必须关闭使能并等待真实静止反馈（板端会拒绝并说明），本页不会自动下发该命令，也不会把示例值当作已确认的驱动器设置。',
      },
      {
        id: 'realtimePositionFeedback',
        name: '实时位置反馈间隔',
        en: 'realtime position feedback interval 11 timed return',
        sendNote: '反馈间隔请求',
        integrated: false,
        summary: '配置主动上报位置反馈的间隔。',
        variants: [
          V('base', '11', 0x11, [
            constant(0x18, '标识'),
            constant(0x36, '子功能'),
            intField('intervalMs', '反馈间隔', 2, { unit: 'ms', default: 100, max: 65535, certainty: 'manual', hint: 'V1.0.5 p66：定时返回信息为 [11][18][信息功能码][定时时间 u16 毫秒][6B]，定时时间 0000 表示停止返回。本条目固定信息功能码 36（实时位置）。' }),
            constant(PROTOCOL_CHECKSUM, '固定校验'),
          ]),
        ],
        note: '手册 V1.0.5 p66：定时返回支持 5.5 节的全部读取功能码（36/3A/…），返回格式与该功能码的读取返回一致，因此 0x36 的 0.1° 与 0x3A 的位定义同样适用于这里。演示中非 0 间隔会按 250 ms 下限产生模拟位置反馈帧。',
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
