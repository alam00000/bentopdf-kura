export type Level =
  | '1b' | '1a' | '2b' | '2u' | '2a' | '3b' | '3u' | '3a' | '4' | '4f' | '4e'
  | 'x1a' | 'x3' | 'x4' | 'x6' | 'e1' | 'vt1' | 'vt3';

export type CheckOnlyLevel = 'x4p' | 'x5g' | 'x5n' | 'x5pg' | 'x6n' | 'x6p' | 'vt2';

export type Bytes = Uint8Array | ArrayBuffer;

export interface KuraOptions {
  ua?: boolean;
  lang?: string;
  password?: string;
  allowVisualRisk?: boolean;
  rasterizePages?: boolean;
  rasterDpi?: number;
  outlineFonts?: boolean;
  attachXml?: Bytes;
  attachXmlName?: string;
  facturxProfile?: string;
  embedSource?: Bytes;
  embedSourceName?: string;
  embedSourceMime?: string;
  outputCondition?: string;
  outputConditionInfo?: string;
  registry?: string;
  destProfile?: Bytes;
  defaultRgb?: Bytes;
  defaultCmyk?: Bytes;
  defaultGray?: Bytes;
  vtRecords?: string;
  profile?: string;
  analyze?: boolean;
  now?: string;
}

export interface Issue {
  code: string;
  detail: string;
  fixed: boolean;
}

export interface Finding {
  code: string;
  detail: string;
}

export interface KuraResult {
  pdf: Uint8Array;
  level: string;
  engine: string;
  issues: Issue[];
  analysis: Finding[];
}

export interface KuraCheckResult {
  compliant: boolean;
  findings: number;
  level: string;
  engine: string;
  issues: Issue[];
  analysis: Finding[];
}

export declare class KuraError extends Error {
  readonly name: 'KuraError';
  readonly code: string;
  readonly suggestedLevel: string | null;
  readonly issues: Issue[];
  constructor(code: string, message: string, extra?: { suggestedLevel?: string | null; issues?: Issue[] });
}

export declare const LEVELS: readonly Level[];
export declare const CHECK_ONLY_LEVELS: readonly CheckOnlyLevel[];

export declare function convert(input: Bytes, level?: Level, options?: KuraOptions): Promise<KuraResult>;
export declare function check(input: Bytes, level?: Level | CheckOnlyLevel, options?: KuraOptions): Promise<KuraCheckResult>;
export declare function verifyPassword(input: Bytes, password?: string): Promise<boolean>;
export declare function version(): Promise<string>;
export declare function load(): Promise<void>;
