// Localized download errors; provider responses are never translated as code.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "download_text.h"
#include <algorithm>
#include <string_view>
#include <utility>

std::string download_remaining_text(std::int64_t seconds, bool italian) {
    const std::string label = italian ? "Tempo rimanente: " : "Time remaining: ";
    if (seconds < 0) return label + (italian ? "in stima…" : "estimating…");
    if (seconds < 60) return label + (italian ? "meno di 1 min" : "less than 1 min");

    // Round up without adding to seconds: even a very large stalled-transfer
    // estimate stays safe, and the interface never promises an earlier finish.
    const std::int64_t minutes = seconds / 60 + (seconds % 60 != 0);
    std::string value;
    if (minutes >= 24 * 60) {
        const std::int64_t hours = minutes / 60 + (minutes % 60 != 0);
        value = std::to_string(hours / 24) + (italian ? " g" : " d");
        if (hours % 24) value += " " + std::to_string(hours % 24) + " h";
    } else if (minutes >= 60) {
        value = std::to_string(minutes / 60) + " h";
        if (minutes % 60) value += " " + std::to_string(minutes % 60) + " min";
    } else {
        value = std::to_string(minutes) + " min";
    }
    return label + "~" + value;
}

std::string download_connections_text(int peers, int seeders, bool italian) {
    if (peers < 0 && seeders < 0) return {};
    if (peers < 0)
        return (italian ? "Seeder connessi: " : "Connected seeders: ") + std::to_string(seeders);
    std::string value = (italian ? "Peer connessi: " : "Connected peers: ") + std::to_string(peers);
    if (seeders >= 0) value += (italian ? " · seeder: " : " · seeders: ") + std::to_string(seeders);
    return value;
}

std::string download_error_text(const std::string& error, bool italian) {
    if (!italian || error.empty()) return error;
    using Pair = std::pair<std::string_view, std::string_view>;
    static constexpr Pair translations[] = {
        {"Download failed. You can retry.", "Riprova il download."},
        {"Download storage is not available.", "La cartella dei download non è disponibile."},
        {"The download folder is not available.", "La cartella dei download non è disponibile."},
        {"The download storage path is invalid", "La cartella dei download non è valida."},
        {"Insufficient storage for this download", "Spazio insufficiente per completare il download."},
        {"Could not write the download to storage", "Impossibile scrivere il download sulla memoria della PS5."},
        {"Could not start the PS5 download writer", "Impossibile avviare il processo di scrittura dei download PS5."},
        {"The PS5 download writer stopped before confirming the data", "Il processo di scrittura PS5 si è interrotto prima di confermare i dati."},
        {"The download could not be created. Check the available storage space.", "Impossibile creare il download. Controlla lo spazio disponibile."},
        {"The limit of 128 downloads has been reached. Delete a video to add another.", "Hai raggiunto il limite di 128 download. Elimina un video per aggiungerne un altro."},
        {"The download state could not be saved.", "Impossibile salvare lo stato del download."},
        {"Could not save the download checkpoint", "Impossibile salvare l'avanzamento del download."},
        {"Could not save the torrent download checkpoint", "Impossibile salvare l'avanzamento del download torrent."},
        {"Could not save the HLS download checkpoint", "Impossibile salvare l'avanzamento del download HLS."},
        {"The download folder could not be deleted.", "Impossibile eliminare la cartella del download."},
        {"This download cannot be deleted.", "Impossibile eliminare questo download."},
        {"The delete request could not be saved.", "Impossibile salvare la richiesta di eliminazione."},
        {"The downloaded file is missing or incomplete. Download it again.", "Il file scaricato è assente o incompleto. Scaricalo di nuovo."},
        {"The downloaded file is incomplete or cannot be saved.", "Il file scaricato è incompleto o non può essere salvato."},
        {"The video was written, but its completion state could not be saved. Download it again.", "Il video è stato scritto, ma il completamento non è stato salvato. Scaricalo di nuovo."},
        {"The video metadata is not valid for downloading.", "I metadati del video non sono validi per il download."},
        {"This source does not support local downloads.", "Questa sorgente non supporta i download locali."},
        {"The source configuration is too large.", "La configurazione della sorgente è troppo grande."},
        {"An internal error interrupted the download. You can retry.", "Un errore interno ha interrotto il download. Puoi riprovare."},
        {"Live streams cannot be downloaded for offline viewing.", "Le dirette non possono essere scaricate per la visione offline."},
        {"Offline download requires a finite file length; live streams are not supported", "La sorgente deve avere una durata e una dimensione finite. Le dirette non sono supportate."},
        {"Live HLS streams cannot be downloaded for offline viewing", "Le dirette HLS non possono essere scaricate per la visione offline."},
        {"Encrypted HLS and DRM sources are not supported for offline downloads", "Le sorgenti HLS cifrate o protette da DRM non supportano il download offline."},
        {"This stream type cannot be downloaded for offline viewing", "Questo tipo di sorgente non può essere scaricato per la visione offline."},
        {"This source has no direct HTTP or HTTPS media URL", "Questa sorgente non fornisce un video diretto HTTP o HTTPS."},
        {"This source does not provide a downloadable media file", "La sorgente non restituisce un file multimediale scaricabile."},
        {"The downloaded file does not contain playable audio or video", "Il file scaricato non contiene audio o video riproducibile."},
        {"The selected torrent file does not contain playable audio or video", "Il file torrent selezionato non contiene audio o video riproducibile."},
        {"The source returned an invalid file length", "La sorgente ha restituito una dimensione del file non valida."},
        {"The source returned compressed bytes that cannot be safely resumed", "La sorgente restituisce dati compressi che non consentono una ripresa affidabile."},
        {"The source returned an incorrect download byte range", "La sorgente ha restituito un intervallo di dati errato."},
        {"Download response headers are too large", "La risposta della sorgente contiene intestazioni troppo grandi."},
        {"The source sent more bytes than its declared file length", "La sorgente ha inviato più dati della dimensione dichiarata."},
        {"The download source cannot resume this file safely", "Questa sorgente non consente di riprendere il file in modo affidabile."},
        {"The source split the file into ranges without a safe resume validator", "La sorgente non permette di verificare i dati necessari a riprendere il download."},
        {"The download source stopped responding", "La sorgente ha smesso di rispondere. Puoi riprovare."},
        {"The file download was interrupted before all bytes arrived", "La connessione si è interrotta prima di ricevere il file completo."},
        {"Could not finalize the complete downloaded file", "Impossibile completare il salvataggio del file scaricato."},
        {"The source returned too many incomplete byte ranges", "La sorgente ha restituito troppi intervalli di dati incompleti."},
        {"Could not initialize the download connection", "Impossibile avviare la connessione per il download."},
        {"Could not initialize download headers", "Impossibile preparare la richiesta di download."},
        {"Could not identify the download source", "Impossibile identificare la sorgente del download."},
        {"This source does not provide a valid torrent hash", "La sorgente non fornisce un identificatore torrent valido."},
        {"Could not obtain the torrent file list", "Impossibile ottenere l'elenco dei file del torrent."},
        {"The selected file is not available in this torrent", "Il file selezionato non è disponibile in questo torrent."},
        {"Could not open the selected torrent file", "Impossibile aprire il file torrent selezionato."},
        {"The torrent download stopped before the file was complete", "Il download torrent si è fermato prima di completare il file."},
        {"The torrent peers stopped sending data", "I peer del torrent hanno smesso di inviare dati."},
        {"The torrent download is incomplete", "Il download torrent è incompleto. Puoi riprovare."},
        {"Could not finalize the torrent download", "Impossibile completare il salvataggio del torrent."},
        {"This HLS playlist is too complex for an offline download", "Questa playlist HLS è troppo complessa per il download offline."},
        {"The HLS playlists exceed the offline manifest limit", "Le playlist HLS superano il limite previsto per il download offline."},
        {"The HLS playlist contains a recursive reference", "La playlist HLS contiene un riferimento circolare non valido."},
        {"This source did not return an HLS playlist", "La sorgente non ha restituito una playlist HLS valida."},
        {"The HLS master playlist is incomplete", "La playlist HLS principale è incompleta."},
        {"Could not open the HLS playlist", "Impossibile aprire la playlist HLS."},
        {"Could not retrieve the complete HLS playlist", "Impossibile scaricare la playlist HLS completa."},
        {"The HLS playlist uses an unsupported media URL", "La playlist HLS contiene un indirizzo multimediale non supportato."},
        {"The HLS initialization segment has an unsupported URL", "Il segmento iniziale HLS usa un indirizzo non supportato."},
        {"The HLS playlist uses an unsupported segment URL", "Un segmento della playlist HLS usa un indirizzo non supportato."},
        {"The HLS playlist is missing a media segment", "Manca un segmento multimediale nella playlist HLS."},
        {"The HLS playlist contains an invalid segment duration", "La playlist HLS contiene un segmento con durata non valida."},
        {"The HLS playlist has a segment without a finite duration", "La playlist HLS contiene un segmento senza durata finita."},
        {"Could not open this HLS video", "Impossibile aprire questo video HLS."},
        {"Could not read all HLS media tracks", "Impossibile leggere tutte le tracce del video HLS."},
        {"The Matroska offline container is unavailable", "Il formato Matroska per i video offline non è disponibile."},
        {"An HLS audio or subtitle track cannot be preserved in the offline container", "Una traccia audio o sottotitoli HLS non può essere conservata nel file offline."},
        {"This HLS source contains no downloadable audio or video", "Questa sorgente HLS non contiene audio o video scaricabile."},
        {"This HLS stream cannot be saved without changing its media tracks", "Questo video HLS non può essere salvato mantenendo le sue tracce originali."},
        {"An HLS segment could not be downloaded completely", "Un segmento HLS non è stato scaricato completamente."},
        {"The HLS download ended before every segment was saved", "Il download HLS si è interrotto prima di salvare tutti i segmenti."},
        {"An HLS segment was missing or incomplete", "Un segmento HLS è assente o incompleto."},
        {"The saved HLS video is shorter than the complete playlist", "Il video HLS salvato è più breve della playlist completa."},
        {"Could not finalize the offline video container", "Impossibile completare il contenitore del video offline."},
        {"Could not finalize the complete offline video", "Impossibile completare il salvataggio del video offline."},
        {"The downloaded HLS container could not be opened", "Impossibile aprire il file HLS scaricato."},
        {"Not enough memory to continue this download", "Memoria insufficiente per continuare il download."},
        {"The download could not be completed", "Impossibile completare il download. Puoi riprovare."}
    };
    for (const auto& [english, translated] : translations)
        if (error == english) return std::string(translated);
    constexpr std::string_view http_prefix = "Download source returned HTTP ";
    if (error.starts_with(http_prefix)) {
        const auto code = std::string_view(error).substr(http_prefix.size());
        if (!code.empty() && code.size() <= 3 &&
            std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return "La sorgente ha risposto con HTTP " + std::string(code) + ".";
    }
    return "Riprova il download o scegli un'altra sorgente.";
}
