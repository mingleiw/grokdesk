// SIMULATED DATA — fabricated sample deployment logs for the grokdesk demo.
// Not from any real system. Live mode (?live) does not use this file.
window.SAMPLE_LOGS = {
  healthy: [
    "[SIMULATED] 2026-10-13T09:12:01Z ci  pipeline #4812 checkout-api: build ok (sha 9f3c2e1)",
    "2026-10-13T09:12:44Z ci  image pushed registry.local/checkout-api:v2.14.0",
    "2026-10-13T09:13:02Z k8s deployment.apps/checkout-api image updated -> v2.14.0",
    "2026-10-13T09:13:30Z k8s Waiting for rollout: 3 of 3 updated replicas are available",
    "2026-10-13T09:13:31Z k8s deployment \"checkout-api\" successfully rolled out",
    "2026-10-13T09:13:40Z probe readiness OK pods=3/3 p95=142ms err_rate=0.02%"
  ],
  failed: [
    "[SIMULATED] 2026-10-13T10:02:11Z ci  pipeline #4820 checkout-api: build ok (sha a71d0b4)",
    "2026-10-13T10:02:50Z ci  image pushed registry.local/checkout-api:v2.15.0",
    "2026-10-13T10:03:05Z k8s deployment.apps/checkout-api image updated -> v2.15.0 (previous: v2.14.0)",
    "2026-10-13T10:03:09Z app checkout-api-7c9f-x2k4p INFO running migrations: 0041_add_index_users, 0042_add_fraud_score_to_orders",
    "2026-10-13T10:03:09Z app checkout-api-7c9f-x2k4p INFO migration 0041_add_index_users applied (0.8s)",
    "2026-10-13T10:03:10Z app checkout-api-7c9f-x2k4p INFO migration 0042: ALTER TABLE orders ADD COLUMN fraud_score NUMERIC NOT NULL DEFAULT 0",
    "2026-10-13T10:03:40Z db  WARN waiting for AccessExclusiveLock on relation \"orders\" (held by pid 18872, autovacuum)",
    "2026-10-13T10:04:10Z app checkout-api-7c9f-x2k4p ERROR migration 0042_add_fraud_score_to_orders failed: canceling statement due to lock timeout (lock_timeout=60s) on table \"orders\"",
    "2026-10-13T10:04:10Z app checkout-api-7c9f-x2k4p FATAL startup aborted: pending migrations; exiting with code 1",
    "2026-10-13T10:04:21Z k8s Warning Unhealthy pod/checkout-api-7c9f-x2k4p Readiness probe failed: HTTP probe failed with statuscode: 503",
    "2026-10-13T10:04:52Z k8s Warning BackOff pod/checkout-api-7c9f-x2k4p Back-off restarting failed container (CrashLoopBackOff) restarts=4",
    "2026-10-13T10:05:05Z k8s Warning BackOff pod/checkout-api-7c9f-m8q1z CrashLoopBackOff restarts=3",
    "2026-10-13T10:08:05Z k8s error: deployment \"checkout-api\" exceeded its progress deadline",
    "2026-10-13T10:08:06Z ci  ERROR rollout status failed — DEPLOY FAILED (v2.15.0)"
  ],
  resolved: [
    "2026-10-13T10:09:30Z ci  [Grok Bot suggestion accepted] kubectl rollout undo deployment/checkout-api",
    "2026-10-13T10:09:58Z k8s deployment.apps/checkout-api rolled back -> v2.14.0",
    "2026-10-13T10:10:20Z k8s deployment \"checkout-api\" successfully rolled out",
    "2026-10-13T10:10:31Z probe readiness OK pods=3/3 p95=139ms err_rate=0.01%",
    "2026-10-13T10:10:32Z ci  DEPLOY HEALTHY (rolled back to v2.14.0) [SIMULATED]"
  ]
};
