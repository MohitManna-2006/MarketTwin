import { create } from "zustand";

/** The plotted point under the pointer, shared by the panes of one chart stack. */
export const useChartHover = create<{ index: number | null; set: (index: number | null) => void }>(
  (set) => ({
    index: null,
    set: (index) => set((state) => (state.index === index ? state : { index })),
  }),
);
