import { useState } from "react";
import { Input } from "./input";
import { cn } from "@/lib/utils";
import { CaretUpIcon, CaretDownIcon } from "@phosphor-icons/react";

interface NumberInputProps extends Omit<
  React.InputHTMLAttributes<HTMLInputElement>,
  "onChange" | "value" | "type"
> {
  value: number;
  onChange: (value: number) => void;
  min?: number;
  max?: number;
  step?: number;
  fallback?: number;
}

export function NumberInput({
  value,
  onChange,
  min,
  max,
  step,
  fallback,
  className,
  disabled,
  ...props
}: NumberInputProps) {
  const [raw, setRaw] = useState(String(value));
  const [focused, setFocused] = useState(false);

  function clamp(n: number): number {
    let result = n;
    if (min !== undefined) result = Math.max(min, result);
    if (max !== undefined) result = Math.min(max, result);
    return result;
  }

  function snap(n: number): number {
    if (!step) return n;
    return Math.round(n / step) * step;
  }

  function commit(str: string) {
    const parsed = parseFloat(str);
    const resolved = isNaN(parsed)
      ? (fallback ?? min ?? 0)
      : clamp(snap(parsed));
    onChange(resolved);
    setRaw(String(resolved));
  }

  function handleChange(e: React.ChangeEvent<HTMLInputElement>) {
    const val = e.target.value;
    if (/^-?\d*\.?\d*$/.test(val) || val === "" || val === "-") {
      setRaw(val);
    }
  }

  function handleKeyDown(e: React.KeyboardEvent<HTMLInputElement>) {
    if (e.key === "Enter") {
      commit(raw);
      (e.target as HTMLInputElement).blur();
    }
    if (e.key === "ArrowUp" || e.key === "ArrowDown") {
      e.preventDefault();
      const delta = (step ?? 1) * (e.key === "ArrowUp" ? 1 : -1);
      const next = clamp(snap((parseFloat(raw) || value) + delta));
      onChange(next);
      setRaw(String(next));
    }
  }

  function handleStep(direction: "up" | "down") {
    const delta = (step ?? 1) * (direction === "up" ? 1 : -1);
    const base = parseFloat(focused ? raw : String(value));
    const next = clamp(snap((isNaN(base) ? value : base) + delta));
    onChange(next);
    setRaw(String(next));
  }

  return (
    <div className={cn("relative flex items-center", className)}>
      <Input
        {...props}
        type="text"
        inputMode="numeric"
        disabled={disabled}
        value={focused ? raw : String(value)}
        onChange={handleChange}
        onFocus={() => {
          setRaw(String(value));
          setFocused(true);
        }}
        onBlur={() => {
          setFocused(false);
          commit(raw);
        }}
        onKeyDown={handleKeyDown}
        className="w-full h-8 text-sm pr-5"
      />
      <div className="absolute right-0.5 inset-y-0.5 flex flex-col w-4">
        <button
          type="button"
          tabIndex={-1}
          disabled={disabled}
          onMouseDown={(e) => {
            e.preventDefault(); // prevent input blur
            handleStep("up");
          }}
          className="flex-1 flex items-center justify-center rounded-sm text-muted-foreground hover:text-foreground hover:bg-accent disabled:opacity-50 disabled:pointer-events-none"
        >
          <CaretUpIcon weight="fill" className="size-2.5" />
        </button>
        <button
          type="button"
          tabIndex={-1}
          disabled={disabled}
          onMouseDown={(e) => {
            e.preventDefault(); // prevent input blur
            handleStep("down");
          }}
          className="flex-1 flex items-center justify-center rounded-sm text-muted-foreground hover:text-foreground hover:bg-accent disabled:opacity-50 disabled:pointer-events-none"
        >
          <CaretDownIcon weight="fill" className="size-2.5" />
        </button>
      </div>
    </div>
  );
}
