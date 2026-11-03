export type OperatorId = "kmb" | "ctb" | "mtr" | "lrt" | "mtrbus" | "gmb";

export type Choice = {
    value: string;
    label: string;
    meta?: string;
    [key: string]: unknown;
};

export type StopSelection = {
    operator: OperatorId;
    route?: string;
    bound?: string;
    service_type?: string;
    direction?: string;
    direction_label?: string;
    stop_id?: string | number;
    stop_name_tc?: string;
    seq?: string | number;
    line?: string;
    line_name_tc?: string;
    station_code?: string;
    station_id?: string;
    stop_code?: string;
    routeName?: string;
    route_name_tc?: string;
    region?: string;
    route_code?: string;
    route_id?: string | number;
    route_seq?: number;
    confirmed_at?: string;
};

export const OPERATORS: { id: OperatorId; label: string }[] = [
    { id: "kmb", label: "九巴/龍運" },
    { id: "ctb", label: "城巴" },
    { id: "mtr", label: "地鐵" },
    { id: "lrt", label: "輕鐵" },
    { id: "mtrbus", label: "接駁巴" },
    { id: "gmb", label: "小巴" },
];

export const MTR_LINE_NAME: Record<string, string> = {
    AEL: "機場快綫",
    TCL: "東涌綫",
    TML: "屯馬綫",
    TKL: "將軍澳綫",
    EAL: "東鐵綫",
    SIL: "南港島綫",
    TWL: "荃灣綫",
    ISL: "港島綫",
    KTL: "觀塘綫",
    DRL: "迪士尼綫",
};

export const GMB_REGIONS = [
    { id: "HKI", label: "港島" },
    { id: "KLN", label: "九龍" },
    { id: "NT", label: "新界" },
];

/**
 * Same-origin catalog URLs on the clock.
 * Firmware downloads official MTR CSVs at runtime → LittleFS (not flash-embed),
 * then serves here so the phone browser avoids opendata CORS.
 */
export const DATA_BASE = "/data";
