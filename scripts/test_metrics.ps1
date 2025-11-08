# Test script for complete decode->render->metrics pipeline
# Copyright (c) 2025 RetroVue

Write-Host "`n=============================================================="
Write-Host "Phase 3 Pipeline Test"
Write-Host "==============================================================`n"

Write-Host "[1/4] Starting playout engine..."
$process = Start-Process -FilePath ".\build\Debug\retrovue_playout.exe" `
                         -ArgumentList "--port 50051" `
                         -PassThru `
                         -WindowStyle Hidden

Start-Sleep -Seconds 3

Write-Host "[2/4] Running gRPC tests..."
echo "" | python scripts\test_server.py

Write-Host "`n[3/4] Testing HTTP metrics endpoint..."
try {
    $metrics = Invoke-WebRequest -Uri "http://localhost:9308/metrics" `
                                 -UseBasicParsing `
                                 -TimeoutSec 5
    
    Write-Host "[SUCCESS] Metrics endpoint responding" -ForegroundColor Green
    Write-Host "`nSample metrics:" -ForegroundColor Cyan
    $metrics.Content -split "`n" | Select-Object -First 15 | ForEach-Object {
        Write-Host "  $_"
    }
} catch {
    Write-Host "[FAIL] Metrics endpoint not responding: $_" -ForegroundColor Red
}

Write-Host "`n[4/4] Cleaning up..."
Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue

Write-Host "`n==============================================================
"
Write-Host "Test complete!" -ForegroundColor Green
Write-Host "==============================================================`n"

