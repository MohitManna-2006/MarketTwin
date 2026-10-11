import { useCallback, useEffect, useMemo, useRef, useSyncExternalStore } from "react";

export interface PagedRows<T> {
  rowCount: number;
  getRow: (index: number) => T | undefined;
  onRangeChange: (start: number, end: number) => void;
  isPending: boolean;
  error: unknown;
  retry: () => void;
}

export interface Page<T> {
  total: number;
  rows: T[];
}

type Fetcher<T> = (offset: number, limit: number) => Promise<Page<T>>;

/** The pages of one data set that have been looked at so far. */
class PageStore<T> {
  private pages = new Map<number, T[]>();
  private pending = new Set<number>();
  private listeners = new Set<() => void>();
  private version = 0;
  total: number | null = null;
  error: unknown = null;

  constructor(private readonly pageSize: number) {}

  subscribe = (listener: () => void) => {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  };

  getVersion = () => this.version;

  private changed() {
    this.version += 1;
    for (const listener of this.listeners) listener();
  }

  row(index: number): T | undefined {
    return this.pages.get(Math.floor(index / this.pageSize))?.[index % this.pageSize];
  }

  load(page: number, fetch: Fetcher<T>) {
    if (this.pages.has(page) || this.pending.has(page)) return;
    this.pending.add(page);
    fetch(page * this.pageSize, this.pageSize)
      .then((result) => {
        this.pages.set(page, result.rows);
        this.total = result.total;
        this.error = null;
      })
      .catch((cause: unknown) => {
        this.error = cause;
      })
      .finally(() => {
        this.pending.delete(page);
        this.changed();
      });
  }

  clearError() {
    this.error = null;
    this.changed();
  }
}

/**
 * Rows of a table that lives on the server. Pages are fetched for the rows on screen and
 * kept, so scrolling a hundred thousand records never holds more than what was looked at.
 * `resetKey` identifies the data set: when it changes, everything loaded is dropped.
 */
export function usePagedRows<T>(
  resetKey: string,
  pageSize: number,
  fetchPage: Fetcher<T>,
): PagedRows<T> {
  // A new data set gets a new store; the old one, and any request still in flight for
  // it, is simply forgotten.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const store = useMemo(() => new PageStore<T>(pageSize), [resetKey, pageSize]);
  const version = useSyncExternalStore(store.subscribe, store.getVersion);
  const fetcher = useRef(fetchPage);
  useEffect(() => {
    fetcher.current = fetchPage;
  });

  useEffect(() => {
    store.load(0, (offset, limit) => fetcher.current(offset, limit));
  }, [store]);

  const onRangeChange = useCallback(
    (start: number, end: number) => {
      for (let page = Math.floor(start / pageSize); page <= Math.floor(end / pageSize); page += 1) {
        store.load(page, (offset, limit) => fetcher.current(offset, limit));
      }
    },
    [store, pageSize],
  );

  const retry = useCallback(() => {
    store.clearError();
    store.load(0, (offset, limit) => fetcher.current(offset, limit));
  }, [store]);

  return useMemo(
    () => ({
      rowCount: store.total ?? 0,
      // A new accessor whenever a page arrives, so the table re-reads its rows.
      getRow: (index: number) => store.row(index),
      onRangeChange,
      isPending: store.total === null && store.error === null,
      error: store.error,
      retry,
    }),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [store, version, onRangeChange, retry],
  );
}
