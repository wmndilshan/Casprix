# CxThreadPool Worker Patch

To support suspended tasks in the Casprix net2 layer, the following patch must be applied to `runtime/net/threadpool.c`. This ensures that `pending_count` is not decremented when a task yields for I/O, as it will be re-incremented when the task is re-scheduled.

```diff
--- runtime/net/threadpool.c
+++ runtime/net/threadpool.c
@@ -142,8 +142,10 @@
         CxTask* t = (CxTask*)cx_mpsc_dequeue(tp->inject_q);
         if (t) {
             if (cx_deque_push(&me->deque, tp->arena, t) != 0) {
+                extern __thread bool tls_suspend_flag; tls_suspend_flag = false;
                 t->fn(t->arg);
-                atomic_fetch_sub_explicit(&tp->pending_count, 1, memory_order_acq_rel);
+                if (!tls_suspend_flag)
+                    atomic_fetch_sub_explicit(&tp->pending_count, 1, memory_order_acq_rel);
             }
             continue;
         }
@@ -155,8 +157,10 @@
             }
         }
         if (t) {
+            extern __thread bool tls_suspend_flag; tls_suspend_flag = false;
             t->fn(t->arg);
-            atomic_fetch_sub_explicit(&tp->pending_count, 1, memory_order_acq_rel);
+            if (!tls_suspend_flag)
+                atomic_fetch_sub_explicit(&tp->pending_count, 1, memory_order_acq_rel);
             continue;
         }
         if (atomic_load_explicit(&tp->pending_count, memory_order_acquire) == 0) {
```

## Rationale
The net2 layer uses stackful coroutines that can suspend on I/O. When a task suspends, it is no longer "running" on the current worker, but it is not "completed" either. The reactor will re-enqueue it later, at which point `pending_count` will be incremented again. If we decrement it here upon suspension, we would double-decrement (or under-count) the total work in flight.
