- **Timing.json**  
  Grafana dashboard focused on MasterClock health.  
  Panels:  
  - Frame gap per channel (±20 ms band)  
  - Corrections/sec via `rate(retrovue_clock_corrections_total[1m])`  
  - `retrovue_clock_jitter_ms_p95`  
  
  *Import:* Grafana → “Dashboards” → “Import” → Upload `grafana/Timing.json`, select your Prometheus data source.


