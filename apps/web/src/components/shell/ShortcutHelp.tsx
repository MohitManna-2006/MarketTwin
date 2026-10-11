import { Dialog } from "@/components/ui/Dialog";

const GROUPS: { title: string; note?: string; keys: [string, string][] }[] = [
  {
    title: "Anywhere",
    keys: [
      ["?", "Open this panel"],
      ["/", "Focus the filter of the current table"],
      ["Esc", "Close a dialog or drawer"],
    ],
  },
  {
    title: "Timeline",
    note: "Work in the Market Microscope, the Agent Inspector, and a seed pair, unless you are typing in a field.",
    keys: [
      ["Space", "Play or pause playback"],
      ["←  /  →", "Previous or next recorded sample"],
      ["Shift ←  /  →", "Back or forward 60 samples"],
      ["Home  /  End", "First or last recorded sample"],
      ["[  /  ]", "Slower or faster playback"],
    ],
  },
  {
    title: "Tables",
    note: "With the table focused.",
    keys: [
      ["↑  /  ↓", "Select the previous or next row"],
      ["PgUp  /  PgDn", "Move twenty rows"],
      ["Enter", "Open the selected row"],
    ],
  },
  {
    title: "Charts",
    keys: [
      ["Click", "Move the time cursor to the latest sample at or before that time"],
      ["Drag", "Zoom to the selected time range"],
      ["Wheel", "Zoom around the pointer"],
      ["Shift + drag", "Pan"],
      ["Double-click", "Reset the zoom"],
    ],
  },
];

export function ShortcutHelp({
  open,
  onOpenChange,
}: {
  open: boolean;
  onOpenChange: (open: boolean) => void;
}) {
  return (
    <Dialog open={open} onOpenChange={onOpenChange} title="Keyboard and pointer" width={560}>
      <div className="flex flex-col">
        {GROUPS.map((group) => (
          <section
            key={group.title}
            className="border-b border-line-subtle px-3 py-2 last:border-b-0"
          >
            <h3 className="label mb-1 text-fg-2">{group.title}</h3>
            {group.note ? <p className="mb-1.5 text-xs text-fg-3">{group.note}</p> : null}
            <dl className="grid grid-cols-[150px_1fr] gap-y-1 text-sm">
              {group.keys.map(([keys, action]) => (
                <div key={keys} className="contents">
                  <dt>
                    <kbd className="num rounded-sm border border-line bg-surface-1 px-1.5 py-px text-xs text-fg-2">
                      {keys}
                    </kbd>
                  </dt>
                  <dd className="text-fg-2">{action}</dd>
                </div>
              ))}
            </dl>
          </section>
        ))}
      </div>
    </Dialog>
  );
}
