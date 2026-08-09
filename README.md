# FVG GRIB Monitor

Applicazione desktop Win32/C++20 per l'inventario di file GRIB2 e la visualizzazione operativa sul Friuli Venezia Giulia.

## Funzioni incluse

- individua ogni messaggio GRIB2 nel file, con offset e dimensione;
- mostra disciplina, categoria e numero del parametro, template di griglia/prodotto, data di riferimento, lead time e dimensione della griglia;
- riconosce CAPE, CIN, precipitazione, temperatura e componenti del vento;
- usa ecCodes per decodificare il solo campo selezionato (simple packing, JPEG2000, PNG, CCSDS, bitmap e altre codifiche supportate) senza bloccare l'interfaccia;
- crea un raster bilineare alla risoluzione della mappa: anche un GFS globale viene campionato direttamente sull'area FVG, anziché disegnare milioni di celle fuori regione;
- ritaglia il raster al vero confine regionale e sovrappone Udine, Trieste, Pordenone e Gorizia con coordinate geografiche reali;
- calcola un indice da 1 a 10 da valori CAPE reali quando il campo selezionato e CAPE.
- include un canale di aggiornamento GitHub automatico e consensuale.

La dashboard usa il materiale Mica di Windows 11 e offre i comandi con icone vettoriali **Apri GRIB**, **Scarica GRIB**, **Aggiorna** (apre il canale GitHub pubblicato) e **About**. Il download interroga NOAA NOMADS per l'ultimo ciclo GFS disponibile, estraendo CAPE e componenti U/V del vento per FVG (12--14 E, 45--47 N).

### Confine regionale

`FvgBoundary.geojson` contiene esclusivamente la geometria del Friuli Venezia Giulia (codice ISTAT 06), in EPSG:4326. È derivato dai confini amministrativi ISTAT al 1 gennaio 2019 pubblicati dal progetto [teamdigitale/confini-amministrativi-istat](https://github.com/teamdigitale/confini-amministrativi-istat/tree/develop/20190101), sorgente ISTAT con licenza CC-BY 4.0. La geometria è stata riproiettata da ETRS89 / UTM 32N e semplificata a 0,0012° per un asset distribuito compatto; i metadati e l'attribuzione restano nell'asset.

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

## Dipendenza ecCodes

L'inventario nativo legge la struttura standard GRIB2 in un thread di lavoro. ecCodes decodifica poi il campo richiesto, mantenendo disponibili tutti i campi senza espandere in memoria un intero file GFS globale.

Per una compilazione locale con manifest:

```powershell
vcpkg install eccodes:x64-windows
```
