-- Each explicit restart request is independent; historical outcomes are audit
-- records, not a workflow lock. The per-user idempotency constraint remains.
DROP INDEX restart_operations_one_pending_per_device;
