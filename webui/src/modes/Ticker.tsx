import { mdiBusStop, mdiChevronLeft, mdiDelete, mdiFormatFont } from "@mdi/js";
import {
    type Component,
    For,
    Show,
    createMemo,
    createSignal,
} from "solid-js";

import { Icon } from "../components/Icon";
import { Toast } from "../components/Toast";
import { Tooltip } from "../components/Tooltip";
import { SidebarSection } from "../extensions/WebUI";
import { WebSocketWS } from "../extensions/WebSocket";
import { FontsList } from "../services/Fonts";
import {
    buildSelection,
    filterChoices,
    loadDirs,
    loadRoutes,
    loadStops,
    type CatalogRaw,
} from "./eta/catalog";
import {
    GMB_REGIONS,
    OPERATORS,
    type Choice,
    type OperatorId,
    type StopSelection,
} from "./eta/types";

export const name = "Ticker";

const [getFont, setFont] = createSignal<string>("");
const [getMessage, setMessage] = createSignal<string>("");
const [getStop, setStop] = createSignal<StopSelection | null>(null);

export const receiver = (json: { font?: string; message?: string; stop?: StopSelection }) => {
    json?.font !== undefined && setFont(json.font);
    json?.message !== undefined && setMessage(json.message);
    if (json?.stop !== undefined) {
        setStop(json.stop && Object.keys(json.stop).length ? json.stop : null);
    }
};

const stopTitle = (s: StopSelection | null) => {
    if (!s) return "未揀站";
    const route = s.route || s.line || s.routeName || s.route_code || "";
    const nameTc = s.stop_name_tc || "";
    return [route, nameTc].filter(Boolean).join(" · ") || "已存站";
};

export const Sidebar: Component = () => {
    const { toast } = Toast();

    const handleFont = (font: string) => {
        setFont(font);
        WebSocketWS.send(
            JSON.stringify({
                [name]: { font: getFont() },
            }),
        );
    };

    const handleClear = () => {
        WebSocketWS.send(JSON.stringify({ [name]: { clear: true } }));
        setStop(null);
        setMessage("No stop");
        toast("已清除站 · 唔再拉 ETA");
    };

    return (
        <SidebarSection>
            <div class="action grid-cols-[--spacing(4)_1fr]">
                <Icon path={mdiBusStop} />
                <div class="min-w-0">
                    <div class="truncate text-sm font-semibold">{stopTitle(getStop())}</div>
                    <Show when={getStop()?.direction_label}>
                        <div class="truncate text-xs opacity-70">{getStop()?.direction_label}</div>
                    </Show>
                    <Show when={getMessage()}>
                        <div class="mt-1 truncate font-mono text-xs opacity-80">{getMessage()}</div>
                    </Show>
                    <p class="mt-2 text-xs opacity-60">主畫面揀站 · Confirm 寫入時鐘</p>
                    <Show when={!!getStop()}>
                        <button
                            type="button"
                            class="action-negative mt-2 !min-h-10 w-full !normal-case tracking-normal"
                            onclick={handleClear}
                        >
                            <Icon path={mdiDelete} />
                            清除站
                        </button>
                    </Show>
                </div>
            </div>
            <div class="action grid-cols-[--spacing(4)_1fr]">
                <Icon path={mdiFormatFont} />
                <Tooltip text="Character availability may vary">
                    <select
                        class="w-full"
                        value={getFont()}
                        onchange={(e) => handleFont(e.currentTarget.value)}
                    >
                        <For each={FontsList()}>{(font) => <option>{font}</option>}</For>
                    </select>
                </Tooltip>
            </div>
        </SidebarSection>
    );
};

export const Main: Component = () => {
    const { toast } = Toast();
    const [step, setStep] = createSignal(1);
    const [status, setStatus] = createSignal("");
    const [busy, setBusy] = createSignal(false);

    const [operator, setOperator] = createSignal<OperatorId | "">("");
    const [operatorLabel, setOperatorLabel] = createSignal("");
    const [region, setRegion] = createSignal("");
    const [routeItems, setRouteItems] = createSignal<Choice[]>([]);
    const [routeValue, setRouteValue] = createSignal("");
    const [routeLabel, setRouteLabel] = createSignal("");
    const [routeQuery, setRouteQuery] = createSignal("");
    const [dirs, setDirs] = createSignal<Choice[]>([]);
    const [dirValue, setDirValue] = createSignal("");
    const [dirLabel, setDirLabel] = createSignal("");
    const [pickedDir, setPickedDir] = createSignal<Choice | null>(null);
    const [stops, setStops] = createSignal<Choice[]>([]);
    const [stopValue, setStopValue] = createSignal("");
    const [stopQuery, setStopQuery] = createSignal("");
    const [pickedStop, setPickedStop] = createSignal<Choice | null>(null);
    const [raw, setRaw] = createSignal<CatalogRaw>({});
    const [selection, setSelection] = createSignal<StopSelection | null>(null);

    const filteredRoutes = createMemo(() => filterChoices(routeItems(), routeQuery()));
    const filteredStops = createMemo(() => filterChoices(stops(), stopQuery()));

    const canNext = createMemo(() => {
        if (busy()) return false;
        if (step() === 1) return !!operator();
        if (step() === 2) return !!routeValue();
        if (step() === 3) return !!dirValue();
        if (step() === 4) return !!stopValue();
        if (step() === 5) return !!selection();
        return false;
    });

    const rebuildSelection = (stop: Choice | null = pickedStop(), dir: Choice | null = pickedDir()) => {
        const sel = buildSelection({
            operator: operator() as OperatorId,
            operatorLabel: operatorLabel(),
            region: region(),
            routeValue: routeValue(),
            routeItems: routeItems(),
            dirValue: dirValue(),
            dirLabel: dirLabel(),
            pickedDir: dir,
            pickedStop: stop,
        });
        setSelection(sel);
        return sel;
    };

    const resetFrom = (from: number) => {
        if (from <= 1) {
            setOperator("");
            setOperatorLabel("");
        }
        if (from <= 2) {
            setRegion("");
            setRouteItems([]);
            setRouteValue("");
            setRouteLabel("");
            setRouteQuery("");
            setRaw({});
        }
        if (from <= 3) {
            setDirs([]);
            setDirValue("");
            setDirLabel("");
            setPickedDir(null);
        }
        if (from <= 4) {
            setStops([]);
            setStopValue("");
            setStopQuery("");
            setPickedStop(null);
        }
        if (from <= 5) {
            setSelection(null);
        }
    };

    const enterRoutes = async () => {
        resetFrom(2);
        setBusy(true);
        setStatus(operator() === "gmb" ? "請先揀區域" : "載入路線…");
        try {
            if (operator() !== "gmb") {
                const { items, raw: next } = await loadRoutes(operator() as OperatorId, region(), raw());
                setRaw(next);
                setRouteItems(items);
                setStatus("請揀路線");
            }
            setStep(2);
        } catch (err) {
            setStatus(String((err as Error).message || err));
        } finally {
            setBusy(false);
        }
    };

    const pickRegion = async (id: string) => {
        setRegion(id);
        setBusy(true);
        setStatus("載入路線…");
        try {
            const { items, raw: next } = await loadRoutes("gmb", id, raw());
            setRaw(next);
            setRouteItems(items);
            setStatus("請揀路線");
        } catch (err) {
            setStatus(String((err as Error).message || err));
        } finally {
            setBusy(false);
        }
    };

    const enterDirs = async () => {
        resetFrom(3);
        setBusy(true);
        setStatus("載入方向…");
        try {
            const { dirs: list, raw: next } = await loadDirs(
                operator() as OperatorId,
                routeValue(),
                region(),
                raw(),
            );
            setRaw(next);
            setDirs(list);
            setStatus(list.length ? "請揀方向" : "呢條線暫時冇方向／站資料");
            setStep(3);
        } catch (err) {
            setStatus(String((err as Error).message || err));
        } finally {
            setBusy(false);
        }
    };

    const enterStops = async () => {
        resetFrom(4);
        setBusy(true);
        setStatus("載入站…");
        try {
            const list = await loadStops(
                operator() as OperatorId,
                routeValue(),
                dirValue(),
                pickedDir(),
                raw(),
            );
            setStops(list);
            setStatus(list.length ? "請揀站" : "呢個方向冇站");
            setStep(4);
        } catch (err) {
            setStatus(String((err as Error).message || err));
        } finally {
            setBusy(false);
        }
    };

    const enterConfirm = () => {
        rebuildSelection();
        setStatus("確認後寫入時鐘");
        setStep(5);
    };

    const doConfirm = () => {
        const sel = rebuildSelection();
        if (!sel) return;
        const payload: StopSelection = {
            ...sel,
            confirmed_at: new Date().toISOString(),
        };
        WebSocketWS.send(
            JSON.stringify({
                [name]: { stop: payload },
            }),
        );
        setStop(payload);
        setStatus("已送出 · 時鐘會拉 ETA");
        toast("站已存入時鐘");
    };

    const onPrimary = () => {
        if (step() === 1) void enterRoutes();
        else if (step() === 2) void enterDirs();
        else if (step() === 3) void enterStops();
        else if (step() === 4) enterConfirm();
        else if (step() === 5) doConfirm();
    };

    const onBack = () => {
        if (step() === 2) {
            resetFrom(2);
            setStep(1);
            setStatus("");
        } else if (step() === 3) {
            resetFrom(3);
            setStep(2);
            setStatus("請揀路線");
        } else if (step() === 4) {
            resetFrom(4);
            setStep(3);
            setStatus("請揀方向");
        } else if (step() === 5) {
            setStep(4);
            setStatus("請揀站");
        }
    };

    const primaryLabel = () => (step() === 5 ? "Confirm／Save" : "下一步");

    return (
        <div class="ticker-eta flex h-full w-full max-w-xl flex-col gap-3 overflow-hidden p-4 text-left">
            <div class="flex items-baseline justify-between gap-3">
                <div>
                    <div class="text-xs font-semibold uppercase tracking-wider opacity-60">Ticker · 揀站</div>
                    <h2 class="text-2xl font-semibold tracking-tight">第 {step()}／5 步</h2>
                </div>
                <Show when={getMessage()}>
                    <div class="max-w-[50%] truncate rounded-lg border px-3 py-2 font-mono text-sm">
                        {getMessage()}
                    </div>
                </Show>
            </div>
            <Show when={status()}>
                <div class="text-sm opacity-70">{status()}</div>
            </Show>

            <div class="min-h-0 flex-1 overflow-y-auto rounded-xl border bg-menu-light p-3 dark:bg-menu-dark">
                <Show when={step() === 1}>
                    <h3 class="mb-3">邊種交通？</h3>
                    <div class="flex flex-wrap gap-2">
                        <For each={OPERATORS}>
                            {(op) => (
                                <button
                                    type="button"
                                    class={`!min-h-10 !normal-case tracking-normal ${operator() === op.id ? "action-positive" : ""}`}
                                    onclick={() => {
                                        setOperator(op.id);
                                        setOperatorLabel(op.label);
                                        setStatus("");
                                    }}
                                >
                                    {op.label}
                                </button>
                            )}
                        </For>
                    </div>
                </Show>

                <Show when={step() === 2}>
                    <h3 class="mb-1">{operator() === "gmb" ? "區域／路線" : "搜路線"}</h3>
                    <p class="mb-3 text-sm opacity-60">{operatorLabel()}</p>
                    <Show when={operator() === "gmb"}>
                        <div class="mb-3 flex flex-wrap gap-2">
                            <For each={GMB_REGIONS}>
                                {(r) => (
                                    <button
                                        type="button"
                                        class={`!min-h-10 !normal-case tracking-normal ${region() === r.id ? "action-positive" : ""}`}
                                        onclick={() => void pickRegion(r.id)}
                                    >
                                        {r.label}
                                    </button>
                                )}
                            </For>
                        </div>
                    </Show>
                    <Show when={operator() !== "gmb" || !!region()}>
                        <input
                            class="mb-3 w-full"
                            type="search"
                            placeholder="搜路線號"
                            value={routeQuery()}
                            oninput={(e) => setRouteQuery(e.currentTarget.value)}
                        />
                        <div class="flex flex-col gap-1">
                            <For each={filteredRoutes()}>
                                {(item) => (
                                    <button
                                        type="button"
                                        class={`!min-h-11 w-full !justify-start !normal-case tracking-normal ${routeValue() === item.value ? "action-positive" : ""}`}
                                        onclick={() => {
                                            setRouteValue(item.value);
                                            setRouteLabel(item.meta ? `${item.label} · ${item.meta}` : item.label);
                                        }}
                                    >
                                        <span class="font-semibold">{item.label}</span>
                                        <Show when={item.meta}>
                                            <span class="ml-2 text-xs opacity-60">{item.meta}</span>
                                        </Show>
                                    </button>
                                )}
                            </For>
                        </div>
                    </Show>
                </Show>

                <Show when={step() === 3}>
                    <h3 class="mb-1">去邊邊？</h3>
                    <p class="mb-3 text-sm opacity-60">{routeLabel()}</p>
                    <div class="flex flex-col gap-1">
                        <For each={dirs()}>
                            {(item) => (
                                <button
                                    type="button"
                                    class={`!min-h-11 w-full !justify-start !normal-case tracking-normal ${dirValue() === item.value ? "action-positive" : ""}`}
                                    onclick={() => {
                                        setDirValue(item.value);
                                        setDirLabel(item.label);
                                        setPickedDir(item);
                                    }}
                                >
                                    {item.label}
                                </button>
                            )}
                        </For>
                    </div>
                </Show>

                <Show when={step() === 4}>
                    <h3 class="mb-1">你喺邊個站？</h3>
                    <p class="mb-3 text-sm opacity-60">{dirLabel()}</p>
                    <input
                        class="mb-3 w-full"
                        type="search"
                        placeholder="搜站名"
                        value={stopQuery()}
                        oninput={(e) => setStopQuery(e.currentTarget.value)}
                    />
                    <div class="flex flex-col gap-1">
                        <For each={filteredStops()}>
                            {(item) => (
                                <button
                                    type="button"
                                    class={`!min-h-11 w-full !justify-start !normal-case tracking-normal ${stopValue() === item.value ? "action-positive" : ""}`}
                                    onclick={() => {
                                        setStopValue(item.value);
                                        setPickedStop(item);
                                        rebuildSelection(item, pickedDir());
                                    }}
                                >
                                    {item.label}
                                </button>
                            )}
                        </For>
                    </div>
                </Show>

                <Show when={step() === 5}>
                    <h3 class="mb-3">確認</h3>
                    <div class="space-y-2 rounded-lg border p-4">
                        <div class="text-sm opacity-70">
                            {operatorLabel()}
                            {selection()?.route || selection()?.line || selection()?.routeName || selection()?.route_code
                                ? ` · ${selection()?.route || selection()?.line || selection()?.routeName || selection()?.route_code}`
                                : ""}
                        </div>
                        <div class="text-2xl font-semibold">{selection()?.stop_name_tc || "—"}</div>
                        <div class="text-sm opacity-70">{dirLabel()}</div>
                    </div>
                    {/* Confirm 後時鐘直打官方 ETA（無 VPS），LED 顯示路線同 HH:MM。用家畫面唔顯示呢句。 */}
                </Show>
            </div>

            <div class="flex gap-2">
                <Show when={step() > 1}>
                    <button
                        type="button"
                        class="!normal-case"
                        disabled={busy()}
                        onclick={onBack}
                    >
                        <Icon path={mdiChevronLeft} />
                        返回
                    </button>
                </Show>
                <button
                    type="button"
                    class="action-positive flex-1 !normal-case"
                    disabled={!canNext()}
                    onclick={onPrimary}
                >
                    {busy() ? "載入中…" : primaryLabel()}
                </button>
            </div>
        </div>
    );
};
