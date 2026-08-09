# FVG GRIB Monitor

Applicazione desktop Win32/C++20 per l'inventario di file GRIB2 e la visualizzazione operativa sul Friuli Venezia Giulia.

## Funzioni incluse

- individua ogni messaggio GRIB2 nel file, con offset e dimensione;
- mostra disciplina, categoria e numero del parametro, template di griglia/prodotto, data di riferimento, lead time e dimensione della griglia;
- riconosce CAPE, CIN, precipitazione, temperatura e componenti del vento;
- decodifica il template GRIB2 5.0 (simple packing) e disegna ogni cella della griglia nel suo punto geografico;
- sovrappone il contorno FVG e Udine, Trieste, Pordenone e Gorizia al raster;
- calcola un indice da 1 a 10 da valori CAPE reali quando il campo selezionato e CAPE.
- include un canale di aggiornamento GitHub automatico e consensuale.

La dashboard usa il materiale Mica di Windows 11 e offre i comandi con icone vettoriali **Apri GRIB**, **Aggiorna** (apre il canale GitHub pubblicato) e **About**.

Il file di esempio `FVG_CAPE_20260809.grib2` contiene cinque messaggi GRIB2; l'applicazione li inventaria senza dipendenze esterne.

## Aprire e compilare

Aprire `FVG-GribMonitor.sln` in Visual Studio 2026 (o Visual Studio 2022 con workload **Desktop development with C++**), selezionare `Debug|x64`, quindi compilare con `Ctrl+Shift+B`.

All'avvio usare **Apri GRIB...** e selezionare un file `.grib` o `.grib2`. Si puo anche passare il percorso del file come unico argomento da riga di comando.

## Aggiornamenti da GitHub

Il workflow `.github/workflows/continuous-update.yml` compila l'app x64 a ogni push su `main` e sostituisce l'asset `FVG-GribMonitor.zip` nella release pre-release `continuous` di `gmy77/GribViewer`.

L'updater non parte in modo invisibile e non usa token: controlla la release pubblica, chiede l'aggiornamento avviandolo manualmente, e conserva l'ID dell'ultima release installata in `%LOCALAPPDATA%\FVG-GribMonitor\update-state.json`.

```powershell
.\Update-FvgGribMonitor.ps1
```

Per rieseguire la stessa release:

```powershell
.\Update-FvgGribMonitor.ps1 -Force
```

## Decodifica completa dei valori

L'inventario nativo legge in sicurezza la struttura standard GRIB2. I valori delle celle possono essere compressi con template diversi: per supportare *tutti* i template operativi (simple packing, JPEG2000, PNG, CCSDS, bitmap e griglie non regolari), la prossima integrazione deve usare **ecCodes** di ECMWF.

Installazione consigliata:

```powershell
vcpkg install eccodes:x64-windows
```

Il passo successivo e collegare ecCodes al lettore per estrarre `values`, costruire il raster reale, e calcolare l'indice da soglie configurabili CAPE, precipitazione, raffiche e shear. L'indice attuale e intenzionalmente etichettato come indicativo: non simula una misura quando il payload non e ancora stato decompresso.
