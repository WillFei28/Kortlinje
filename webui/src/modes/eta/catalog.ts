import {
    DATA_BASE,
    type Choice,
    type OperatorId,
    type StopSelection,
    MTR_LINE_NAME,
} from "./types";

type CsvRow = Record<string, string>;

const csvCache = new Map<string, CsvRow[]>();
let gmbAll: Record<string, string[]> | null = null;

async function fetchJson<T = unknown>(url: string): Promise<T> {
    const res = await fetch(url, { cache: "no-store" });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return res.json() as Promise<T>;
}

function splitCsvLine(line: string): string[] {
    const out: string[] = [];
    let cur = "";
    let q = false;
    for (let i = 0; i < line.length; i++) {
        const ch = line[i];
        if (ch === '"') {
            if (q && line[i + 1] === '"') {
                cur += '"';
                i++;
            } else q = !q;
        } else if (ch === "," && !q) {
            out.push(cur);
            cur = "";
        } else cur += ch;
    }
    out.push(cur);
    return out;
}

function parseCsv(text: string): CsvRow[] {
    const lines = text.replace(/^\uFEFF/, "").trim().split(/\r?\n/);
    const headers = splitCsvLine(lines[0] || "");
    return lines.slice(1).filter(Boolean).map((line) => {
        const cols = splitCsvLine(line);
        const row: CsvRow = {};
        headers.forEach((h, i) => {
            row[h.trim()] = (cols[i] || "").trim();
        });
        return row;
    });
}

async function loadCsv(name: string): Promise<CsvRow[]> {
    const hit = csvCache.get(name);
    if (hit) return hit;
    // Clock downloads official MTR CSV into LittleFS on demand; 503 = still fetching.
    let lastStatus = 0;
    for (let attempt = 0; attempt < 20; attempt++) {
        const res = await fetch(`${DATA_BASE}/${name}`, { cache: "no-store" });
        lastStatus = res.status;
        if (res.ok) {
            const rows = parseCsv(await res.text());
            csvCache.set(name, rows);
            return rows;
        }
        if (res.status !== 503) break;
        await new Promise((r) => setTimeout(r, 1500));
    }
    throw new Error(`CSV ${name} HTTP ${lastStatus}`);
}

function seqLabel(seq: unknown): string {
    const n = Number(seq);
    return Number.isFinite(n) ? String(Math.trunc(n)) : String(seq);
}

function sortedStops(rows: CsvRow[], seqKey: string): CsvRow[] {
    return rows.slice().sort((a, b) => Number(a[seqKey]) - Number(b[seqKey]));
}

function terminusLabel(rows: CsvRow[], nameKey: string, seqKey: string): string {
    const sorted = sortedStops(rows, seqKey);
    if (!sorted.length) return "";
    return `${sorted[0][nameKey]} → ${sorted[sorted.length - 1][nameKey]}`;
}

export type CatalogRaw = {
    kmbRoutes?: Array<{
        route: string;
        bound: string;
        service_type: string;
        orig_tc: string;
        dest_tc: string;
    }>;
    ctbRoutes?: Array<{ route: string; orig_tc: string; dest_tc: string }>;
    mtrRows?: CsvRow[];
    lrtRows?: CsvRow[];
    mtrBusRoutes?: CsvRow[];
    mtrBusStops?: CsvRow[];
    ctbDirStops?: Record<string, Choice[]>;
};

export async function loadRoutes(
    operator: OperatorId,
    region: string,
    raw: CatalogRaw,
): Promise<{ items: Choice[]; raw: CatalogRaw }> {
    const next = { ...raw };
    if (operator === "kmb") {
        const data = await fetchJson<{ data?: CatalogRaw["kmbRoutes"] }>(
            "https://data.etabus.gov.hk/v1/transport/kmb/route/",
        );
        next.kmbRoutes = data.data || [];
        const routes = [...new Set(next.kmbRoutes.map((r) => r.route))];
        return {
            raw: next,
            items: routes
                .sort((a, b) => a.localeCompare(b, "en", { numeric: true }))
                .map((r) => ({ value: r, label: r })),
        };
    }
    if (operator === "ctb") {
        const data = await fetchJson<{ data?: CatalogRaw["ctbRoutes"] }>(
            "https://rt.data.gov.hk/v2/transport/citybus/route/ctb",
        );
        next.ctbRoutes = data.data || [];
        return {
            raw: next,
            items: next.ctbRoutes
                .map((r) => ({
                    value: r.route,
                    label: r.route,
                    meta: `${r.orig_tc} → ${r.dest_tc}`,
                }))
                .sort((a, b) => a.value.localeCompare(b.value, "en", { numeric: true })),
        };
    }
    if (operator === "mtr") {
        const rows = (await loadCsv("mtr_lines_and_stations.csv")).filter(
            (r) => r["Line Code"] && r.Sequence !== "",
        );
        next.mtrRows = rows;
        const lines = [...new Set(rows.map((r) => r["Line Code"]))].sort();
        return {
            raw: next,
            items: lines.map((l) => ({
                value: l,
                label: MTR_LINE_NAME[l] || l,
                meta: l,
            })),
        };
    }
    if (operator === "lrt") {
        const rows = await loadCsv("light_rail_routes_and_stops.csv");
        next.lrtRows = rows;
        const lines = [...new Set(rows.map((r) => r["Line Code"]))]
            .filter(Boolean)
            .sort((a, b) => a.localeCompare(b, "en", { numeric: true }));
        return {
            raw: next,
            items: lines.map((l) => ({ value: l, label: l })),
        };
    }
    if (operator === "mtrbus") {
        const routes = await loadCsv("mtr_bus_routes.csv");
        const stops = await loadCsv("mtr_bus_stops.csv");
        next.mtrBusStops = stops;
        const byId = new Map<string, CsvRow & { _score: number }>();
        for (const r of routes) {
            const cur = byId.get(r.ROUTE_ID);
            const score = (r.LINE_DOWN ? 2 : 0) + (r.REFERENCE_ID === r.ROUTE_ID ? 1 : 0);
            if (!cur || score > cur._score) byId.set(r.ROUTE_ID, { ...r, _score: score });
        }
        next.mtrBusRoutes = [...byId.values()];
        return {
            raw: next,
            items: next.mtrBusRoutes
                .sort((a, b) => a.ROUTE_ID.localeCompare(b.ROUTE_ID, "en", { numeric: true }))
                .map((r) => ({
                    value: r.ROUTE_ID,
                    label: r.ROUTE_ID,
                    meta: (r.ROUTE_NAME_CHI || "").trim(),
                })),
        };
    }
    if (operator === "gmb") {
        if (!gmbAll) {
            const data = await fetchJson<{ data?: { routes?: Record<string, string[]> } }>(
                "https://data.etagmb.gov.hk/route/",
            );
            gmbAll = data.data?.routes || {};
        }
        const codes = (gmbAll[region] || [])
            .slice()
            .sort((a, b) => a.localeCompare(b, "en", { numeric: true }));
        return {
            raw: next,
            items: codes.map((c) => ({ value: c, label: c })),
        };
    }
    return { raw: next, items: [] };
}

async function loadCtbStopsNamed(route: string, dir: string): Promise<Choice[]> {
    const rs = await fetchJson<{ data?: Array<{ stop: string; seq: number }> }>(
        `https://rt.data.gov.hk/v2/transport/citybus/route-stop/CTB/${encodeURIComponent(route)}/${dir}`,
    );
    const stops = rs.data || [];
    return Promise.all(
        stops.map(async (s) => {
            try {
                const info = await fetchJson<{ data?: { name_tc?: string } }>(
                    `https://rt.data.gov.hk/v2/transport/citybus/stop/${s.stop}`,
                );
                const n = info.data || {};
                return {
                    value: s.stop,
                    label: `${seqLabel(s.seq)}. ${n.name_tc || s.stop}`,
                    name_tc: n.name_tc || "",
                    seq: s.seq,
                    stop: s.stop,
                };
            } catch {
                return {
                    value: s.stop,
                    label: `${seqLabel(s.seq)}. ${s.stop}`,
                    name_tc: "",
                    seq: s.seq,
                    stop: s.stop,
                };
            }
        }),
    );
}

export async function loadDirs(
    operator: OperatorId,
    routeValue: string,
    region: string,
    raw: CatalogRaw,
): Promise<{ dirs: Choice[]; raw: CatalogRaw }> {
    const next = { ...raw, ctbDirStops: { ...(raw.ctbDirStops || {}) } };
    const dirs: Choice[] = [];

    if (operator === "kmb") {
        for (const r of (next.kmbRoutes || []).filter((x) => x.route === routeValue)) {
            dirs.push({
                value: `${r.bound}|${r.service_type}`,
                label: `${r.orig_tc} → ${r.dest_tc}`,
                bound: r.bound,
                service_type: r.service_type,
                direction: r.bound === "I" ? "inbound" : "outbound",
            });
        }
    } else if (operator === "ctb") {
        for (const dir of ["outbound", "inbound"]) {
            const named = await loadCtbStopsNamed(routeValue, dir);
            if (!named.length) continue;
            next.ctbDirStops![dir] = named;
            dirs.push({
                value: dir,
                label: `${named[0].name_tc || named[0].stop} → ${named[named.length - 1].name_tc || named[named.length - 1].stop}`,
            });
        }
    } else if (operator === "mtr") {
        const rows = next.mtrRows || [];
        const uniq = [...new Set(rows.filter((r) => r["Line Code"] === routeValue).map((r) => r.Direction))];
        for (const d of uniq) {
            const subset = rows.filter((r) => r["Line Code"] === routeValue && r.Direction === d);
            dirs.push({ value: d, label: terminusLabel(subset, "Chinese Name", "Sequence") || d });
        }
    } else if (operator === "lrt") {
        const rows = next.lrtRows || [];
        const uniq = [...new Set(rows.filter((r) => r["Line Code"] === routeValue).map((r) => r.Direction))].sort();
        for (const d of uniq) {
            const subset = rows.filter((r) => r["Line Code"] === routeValue && r.Direction === d);
            dirs.push({ value: d, label: terminusLabel(subset, "Chinese Name", "Sequence") || d });
        }
    } else if (operator === "mtrbus") {
        const stops = (next.mtrBusStops || []).filter((s) => s.ROUTE_ID === routeValue);
        const uniq = [...new Set(stops.map((s) => s.DIRECTION))].sort();
        for (const d of uniq) {
            const subset = stops.filter((s) => s.DIRECTION === d);
            dirs.push({ value: d, label: terminusLabel(subset, "STATION_NAME_CHI", "STATION_SEQNO") || d });
        }
    } else if (operator === "gmb") {
        const detail = await fetchJson<{
            data?: Array<{ route_id: number; directions?: Array<{ route_seq: number; orig_tc: string; dest_tc: string }> }>;
        }>(`https://data.etagmb.gov.hk/route/${encodeURIComponent(region)}/${encodeURIComponent(routeValue)}`);
        for (const entry of detail.data || []) {
            for (const d of entry.directions || []) {
                dirs.push({
                    value: `${entry.route_id}|${d.route_seq}`,
                    label: `${d.orig_tc} → ${d.dest_tc}`,
                    route_id: entry.route_id,
                    route_seq: d.route_seq,
                });
            }
        }
    }

    return { dirs: dirs.filter((d) => d.label), raw: next };
}

export async function loadStops(
    operator: OperatorId,
    routeValue: string,
    dirValue: string,
    pickedDir: Choice | null,
    raw: CatalogRaw,
): Promise<Choice[]> {
    if (operator === "kmb") {
        const dir = pickedDir;
        if (!dir) return [];
        const rs = await fetchJson<{ data?: Array<{ stop: string; seq: number }> }>(
            `https://data.etabus.gov.hk/v1/transport/kmb/route-stop/${encodeURIComponent(routeValue)}/${dir.direction}/${dir.service_type}`,
        );
        return Promise.all(
            (rs.data || []).map(async (s) => {
                try {
                    const info = await fetchJson<{ data?: { name_tc?: string } }>(
                        `https://data.etabus.gov.hk/v1/transport/kmb/stop/${s.stop}`,
                    );
                    const n = info.data || {};
                    return {
                        value: s.stop,
                        label: `${seqLabel(s.seq)}. ${n.name_tc || s.stop}`,
                        name_tc: n.name_tc || "",
                        seq: s.seq,
                        stop: s.stop,
                    };
                } catch {
                    return {
                        value: s.stop,
                        label: `${seqLabel(s.seq)}. ${s.stop}`,
                        name_tc: "",
                        seq: s.seq,
                        stop: s.stop,
                    };
                }
            }),
        );
    }
    if (operator === "ctb") {
        return raw.ctbDirStops?.[dirValue] || (await loadCtbStopsNamed(routeValue, dirValue));
    }
    if (operator === "mtr") {
        return sortedStops(
            (raw.mtrRows || []).filter(
                (r) => r["Line Code"] === routeValue && r.Direction === dirValue,
            ),
            "Sequence",
        ).map((r) => ({
            value: r["Station Code"],
            label: `${seqLabel(r.Sequence)}. ${r["Chinese Name"]}`,
            name_tc: r["Chinese Name"],
            station_code: r["Station Code"],
            station_id: r["Station ID"],
            seq: r.Sequence,
        }));
    }
    if (operator === "lrt") {
        return sortedStops(
            (raw.lrtRows || []).filter(
                (r) => r["Line Code"] === routeValue && r.Direction === dirValue,
            ),
            "Sequence",
        ).map((r) => ({
            value: r["Stop ID"],
            label: `${seqLabel(r.Sequence)}. ${r["Chinese Name"]}`,
            name_tc: r["Chinese Name"],
            stop_id: r["Stop ID"],
            stop_code: r["Stop Code"],
            seq: r.Sequence,
        }));
    }
    if (operator === "mtrbus") {
        return sortedStops(
            (raw.mtrBusStops || []).filter(
                (s) => s.ROUTE_ID === routeValue && s.DIRECTION === dirValue,
            ),
            "STATION_SEQNO",
        ).map((s) => ({
            value: s.STATION_ID,
            label: `${seqLabel(s.STATION_SEQNO)}. ${s.STATION_NAME_CHI}`,
            name_tc: s.STATION_NAME_CHI,
            station_id: s.STATION_ID,
            seq: s.STATION_SEQNO,
        }));
    }
    if (operator === "gmb") {
        const dir = pickedDir;
        if (!dir) return [];
        const rs = await fetchJson<{ data?: { route_stops?: Array<{ stop_id: number; stop_seq: number; name_tc: string }> } }>(
            `https://data.etagmb.gov.hk/route-stop/${dir.route_id}/${dir.route_seq}`,
        );
        return (rs.data?.route_stops || []).map((s) => ({
            value: String(s.stop_id),
            label: `${seqLabel(s.stop_seq)}. ${s.name_tc}`,
            name_tc: s.name_tc,
            stop_id: s.stop_id,
            seq: s.stop_seq,
            route_id: dir.route_id,
            route_seq: dir.route_seq,
        }));
    }
    return [];
}

export function buildSelection(args: {
    operator: OperatorId;
    operatorLabel: string;
    region: string;
    routeValue: string;
    routeItems: Choice[];
    dirValue: string;
    dirLabel: string;
    pickedDir: Choice | null;
    pickedStop: Choice | null;
}): StopSelection | null {
    const stop = args.pickedStop;
    const dir = args.pickedDir;
    if (!stop) return null;
    if (args.operator === "kmb") {
        return {
            operator: "kmb",
            route: args.routeValue,
            bound: String(dir?.bound || ""),
            service_type: String(dir?.service_type || "1"),
            stop_id: String(stop.stop || stop.value),
            stop_name_tc: String(stop.name_tc || ""),
            seq: stop.seq as number,
            direction_label: args.dirLabel,
        };
    }
    if (args.operator === "ctb") {
        return {
            operator: "ctb",
            route: args.routeValue,
            direction: args.dirValue,
            stop_id: String(stop.stop || stop.value),
            stop_name_tc: String(stop.name_tc || ""),
            seq: stop.seq as number,
            direction_label: args.dirLabel,
        };
    }
    if (args.operator === "mtr") {
        return {
            operator: "mtr",
            line: args.routeValue,
            line_name_tc: MTR_LINE_NAME[args.routeValue] || args.routeValue,
            direction: args.dirValue,
            direction_label: args.dirLabel,
            station_code: String(stop.station_code || stop.value),
            station_id: String(stop.station_id || ""),
            stop_name_tc: String(stop.name_tc || ""),
            seq: stop.seq as string,
        };
    }
    if (args.operator === "lrt") {
        return {
            operator: "lrt",
            line: args.routeValue,
            direction: args.dirValue,
            direction_label: args.dirLabel,
            stop_id: String(stop.stop_id || stop.value),
            stop_code: String(stop.stop_code || ""),
            stop_name_tc: String(stop.name_tc || ""),
            seq: stop.seq as string,
        };
    }
    if (args.operator === "mtrbus") {
        const meta = args.routeItems.find((r) => r.value === args.routeValue)?.meta || "";
        return {
            operator: "mtrbus",
            routeName: args.routeValue,
            route_name_tc: meta,
            direction: args.dirValue,
            direction_label: args.dirLabel,
            station_id: String(stop.station_id || stop.value),
            stop_name_tc: String(stop.name_tc || ""),
            seq: stop.seq as string,
        };
    }
    if (args.operator === "gmb") {
        return {
            operator: "gmb",
            region: args.region,
            route_code: args.routeValue,
            route_id: (stop.route_id as number) || (dir?.route_id as number),
            route_seq: (stop.route_seq as number) || (dir?.route_seq as number),
            stop_id: stop.stop_id as number,
            stop_name_tc: String(stop.name_tc || ""),
            seq: stop.seq as number,
            direction_label: args.dirLabel,
        };
    }
    return null;
}

export function filterChoices(items: Choice[], q: string): Choice[] {
    const needle = q.trim().toLowerCase();
    if (!needle) return items;
    return items.filter(
        (r) =>
            r.label.toLowerCase().includes(needle) ||
            r.value.toLowerCase().includes(needle) ||
            (r.meta || "").toLowerCase().includes(needle) ||
            String(r.name_tc || "")
                .toLowerCase()
                .includes(needle),
    );
}
