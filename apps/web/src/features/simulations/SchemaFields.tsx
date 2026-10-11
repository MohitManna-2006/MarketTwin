import { DurationField } from "@/components/ui/DurationField";
import { IntegerField, SelectField } from "@/components/ui/Field";
import type { FieldSpec } from "@/lib/api/types";
import { cx } from "@/lib/cx";

export interface SchemaFieldsProps {
  /** Field descriptions from the API: exactly what the native schema accepts. */
  fields: FieldSpec[];
  value: Record<string, unknown>;
  /** Dotted path of `value` inside the config, for matching validation errors. */
  path: string;
  errors: Map<string, string>;
  onChange: (name: string, value: number | string | null) => void;
  /** Field names to leave out, when a screen edits them elsewhere. */
  omit?: readonly string[];
  columns?: 1 | 2 | 3;
  inline?: boolean;
}

const LABELS: Record<string, string> = {
  ppm: "ppm",
  ns: "",
  id: "",
  ids: "",
  seed: "",
  "minor/tick": "minor / tick",
};

export function fieldLabel(name: string): string {
  return name.replace(/_(ns|ticks|milliticks|ppm|minor)$/, "").replaceAll("_", " ");
}

/**
 * Controls generated from the native config schema. A parameter the core does not accept
 * has no description here, so it has no control.
 */
export function SchemaFields({
  fields,
  value,
  path,
  errors,
  onChange,
  omit = [],
  columns = 2,
  inline,
}: SchemaFieldsProps) {
  const shown = fields.filter((field) => !omit.includes(field.name) && field.name in value);
  return (
    <div
      className={cx(
        "grid gap-x-3 gap-y-2",
        columns === 1 ? "grid-cols-1" : columns === 2 ? "grid-cols-2" : "grid-cols-3",
      )}
    >
      {shown.map((field) => {
        const current = value[field.name];
        const error = errors.get(`${path}.${field.name}`) ?? null;
        const label = fieldLabel(field.name);
        if (field.options) {
          return (
            <SelectField
              key={field.name}
              label={label}
              hint={field.description}
              value={String(current)}
              error={error}
              inline={inline}
              options={field.options.map((option) => ({ value: option, label: option }))}
              onChange={(next) => onChange(field.name, next)}
            />
          );
        }
        if (field.unit === "ns") {
          return (
            <DurationField
              key={field.name}
              label={label}
              hint={field.description}
              value={Number(current)}
              min={field.minimum}
              max={field.maximum}
              error={error}
              inline={inline}
              onChange={(next) => onChange(field.name, next)}
            />
          );
        }
        return (
          <IntegerField
            key={field.name}
            label={label}
            unit={LABELS[field.unit] ?? field.unit}
            hint={field.description}
            value={current === null || current === undefined ? null : Number(current)}
            min={field.minimum}
            max={field.maximum}
            nullable={field.nullable}
            signed={(field.minimum ?? 0) < 0}
            error={error}
            inline={inline}
            onChange={(next) => onChange(field.name, next)}
          />
        );
      })}
    </div>
  );
}

export function FormSection({
  title,
  note,
  actions,
  children,
}: {
  title: string;
  note?: string;
  actions?: React.ReactNode;
  children: React.ReactNode;
}) {
  return (
    <section className="border-b border-line-subtle px-3 py-2.5">
      <header className="mb-2 flex items-baseline gap-2">
        <h3 className="label shrink-0 whitespace-nowrap text-fg-2">{title}</h3>
        {note ? <span className="min-w-0 truncate text-xs text-fg-3">{note}</span> : null}
        <span className="flex-1" />
        {actions ? <span className="shrink-0">{actions}</span> : null}
      </header>
      {children}
    </section>
  );
}
