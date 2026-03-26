# Compile Training Script (Incremental)

Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "  Compiling Full Training Script" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan
Write-Host ""

# Clean old objects
Write-Host "[1/3] Cleaning..." -ForegroundColor Yellow
Remove-Item *.o -ErrorAction SilentlyContinue
Remove-Item train_full.exe -ErrorAction SilentlyContinue

# Compile object files
Write-Host "[2/3] Compiling object files..." -ForegroundColor Yellow

$files = @(
    "runtime/llm/tensor.c",
    "runtime/llm/ops.c",
    "runtime/llm/tokenizer.c",
    "runtime/llm/transformer.c",
    "runtime/llm/checkpoint.c",
    "runtime/llm/backward.c",
    "runtime/llm/training.c"
)

foreach ($file in $files) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($file)
    Write-Host "  Compiling $name..." -NoNewline
    gcc -c $file -I. -O2 -std=c11 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host " OK" -ForegroundColor Green
    } else {
        Write-Host " FAILED" -ForegroundColor Red
        exit 1
    }
}

# Link
Write-Host "[3/3] Linking..." -ForegroundColor Yellow
gcc train_full.c tensor.o ops.o tokenizer.o transformer.o checkpoint.o backward.o training.o `
    -o train_full.exe -std=c11 -lm

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "Compilation successful!" -ForegroundColor Green
    
    # Show file size
    $size = (Get-Item train_full.exe).Length / 1MB
    Write-Host "Binary size: $([math]::Round($size, 2)) MB" -ForegroundColor Cyan
} else {
    Write-Host ""
    Write-Host "Linking failed!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=========================================" -ForegroundColor Cyan
