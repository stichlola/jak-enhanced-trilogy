# Jak Trilogy Enhanced — roadmap

Obiettivo: Jak 1, 2 e 3 come **un unico gioco**, con le stesse migliorie grafiche di Jak 3 Enhanced
su tutti e tre, e due livelli di raccordo nuovi tra un gioco e l'altro.

Ordine: prima il motore unico (un solo programma, un solo menu, un solo salvataggio), poi le
migliorie grafiche gioco per gioco, alla fine i livelli di raccordo.

## Dove siamo

- Il fork è OpenGOAL: un solo `gk.exe` sa già far girare Jak 1, 2 e 3, con il renderer C++
  condiviso (`game/graphics/opengl_renderer`). Le migliorie Enhanced stanno in `EnhancedPost`, ma
  sono agganciate solo al percorso di Jak 3 (`dispatch_buckets_jak3`) e ai sorgenti GOAL di Jak 3
  (`goal_src/jak3`: `time-of-day.gc`, `kernel-defs.gc`, `cty-faction.gc`, …).
- Dati: ci sono tutti e tre i giochi. Jak 1 PAL (`SCES-50361`, `iso_data/jak1`, estratto e
  compilato il 26/10/2026: parte fino al titolo dal nostro `gk`), Jak 2 PAL e Jak 3 PAL.
- Impostazioni Enhanced per gioco: `%APPDATA%\OpenGOAL\jak3\settings\enhanced-settings.json`.

## La scelta di fondo: cosa vuol dire "un motore solo"

I tre giochi hanno **tre motori GOAL diversi**. Jak 2 e Jak 3 sono parenti stretti; Jak 1 è un
motore molto più vecchio, con formati dei livelli, codice degli oggetti e sistema di luce
(mood) diversi.

| Strada | Cosa significa | Costo | Verdetto |
|---|---|---|---|
| **A. Un programma, tre motori** | Un solo eseguibile, finestra, menu, salvataggio e renderer. Il programma passa da un motore GOAL all'altro senza chiudersi. | settimane | **consigliata** |
| B. Jak 2 dentro il motore di Jak 3 | Portare livelli e codice di Jak 2 nel motore di Jak 3 (i formati sono simili) | molti mesi | solo se serve dopo |
| C. Tutto nel motore di Jak 3 | Riscrivere Jak 1 per il motore di Jak 3 | anni | no |

La strada A è sensata anche per la storia: all'inizio di Jak 2 e di Jak 3, Jak perde tutto (armi,
oggetti, progressi). Tra un gioco e l'altro non c'è quasi niente da passare: solo statistiche,
tempo di gioco e opzioni. Per chi gioca, il risultato è un solo gioco continuo.

## Fase 0 — Basi

- [x] Estrarre Jak 1 dalla propria copia (`extractor -g jak1 -e -d -c --proj-path <repo>`,
      ~1 min), compilare e avviarlo dal fork (`gk --game jak1 -- -boot -fakeiso`).
- [ ] Provare anche Jak 2 dal fork, **senza** migliorie, come riferimento.
- [ ] Screenshot di riferimento degli stessi punti nei tre giochi (servono per la coerenza).

## Fase 1 — Un solo gioco (motore unificato)

1. **Avvio "trilogia"**: `gk --game trilogy` apre un menu unico con Nuova partita (dall'inizio di
   Jak 1), Continua e Scegli capitolo (Jak 1 / Jak 2 / Jak 3).
2. **Cambio gioco a caldo**: il runtime ferma il kernel GOAL (thread, memoria, livelli, banche
   audio) e lo riavvia con l'altra versione e gli altri dati, **senza chiudere la finestra**. Il
   renderer si reinizializza per la nuova versione: `init_bucket_renderers_jak1/2/3` e le texture.
   Durante il cambio, uno schermo di caricamento.
3. **Fine di un gioco → inizio del successivo**:
   - Jak 1: dopo il boss finale e i titoli di coda (`levels/finalboss`), si passa a Jak 2;
   - Jak 2: dopo l'outro (`levels/outro`), si passa a Jak 3.

   All'inizio è un passaggio diretto; i raccordi veri arrivano nella fase 4.
4. **Salvataggio trilogia**: un file con il capitolo corrente, il salvataggio nativo di ciascun
   gioco e le statistiche cumulative (celle, orbi, ore).
5. **Impostazioni condivise**: un solo `enhanced-settings.json`, gli stessi comandi, audio e lingua
   per i tre giochi.
6. **Primo prototipo (il passo più piccolo)**: un tasto di debug che, in Jak 2, passa a Jak 3 a
   caldo. Jak 2 e Jak 3 sono già pronti, quindi si può fare subito, senza aspettare Jak 1.

## Fase 2 — Migliorie grafiche su Jak 2

Si parte da Jak 2 perché il motore è quasi uguale a quello di Jak 3.

- [x] Agganciare `EnhancedPost` in `dispatch_buckets_jak2` (prima di minimappa e HUD, i bucket
      dell'oceano). Il resto del renderer (normali, `merc2`, materiali, erba) era già comune.
- [x] Ponti GOAL in `goal_src/jak2`: sole, scena e tuoni (`time-of-day.gc`), lampioni, esplosioni
      (`generic-obs.gc`), Jak e chi gli sta attorno per l'erba (`logic-target.gc`, `nav-enemy.gc`),
      menu (ora, orologio fermo, continue point), `JAK_ENHANCED_START` / `JAK_ENHANCED_HOUR`.
- [x] Zone di Jak 2 nella gradazione (palazzo, oracolo, nascondiglio, miniera, scavi, trivella,
      castello, tomba, nido) e una correzione per gioco: in Jak 2 luce rimbalzata ×0,6 e lampioni
      ×0,45 (livelli più luminosi, neon più forti).
- [ ] Da fare: popolazione e traffico, munizioni, tutto sbloccato, minimappa nascosta.
- [ ] Meteo per zona. Haven intatta (Jak 2) è perfetta per pioggia e temporale di notte.
      Poi Fogne, Foresta, Montagna, Atollo, Palazzo, Strip.
- [ ] Popolazione e traffico: Jak 2 ha il suo gestore del traffico (`city`). Si adatta
      l'impostazione "popolazione" di Jak 3.
- [ ] Upscale delle texture con la cache come per Jak 3.
- [ ] Prova: stessi punti, prima e dopo, fps (log "average … fps").

## Fase 3 — Migliorie grafiche su Jak 1

Qui c'è più lavoro: bucket diversi (`dispatch_buckets_jak1`) e un sistema di luce diverso (mood
a tabelle, cielo e sole propri).

- [ ] Agganciare `EnhancedPost` al percorso di Jak 1: AO, GI, riflessi, raggi di luce, nuvole
      volumetriche, profondità di campo.
- [ ] Ponte GOAL per Jak 1: sole, ora del giorno, livello corrente.
- [ ] Meteo pensato per Jak 1:
      - Sandover e spiaggia: "Sunny" (è il suo look naturale);
      - pioggia: Palude di Boggy Billy, Giungla Proibita;
      - neve che cade (nuova): Snowy Mountain;
      - aria tremolante dal calore: Fire Canyon, Volcanic Crater;
      - nebbia: Misty Island.
- [ ] **Passata di coerenza** sui tre giochi: stessa famiglia di colori, stessa intensità degli
      effetti, stessi preset.

## Fase 4 — Livelli di raccordo

### 4.1 Fine Jak 1 → inizio Jak 2

- Si usa la scena d'apertura di Jak 2, già pronta (`goal_src/jak2/levels/intro`: il Rift Rider, il
  portale, il vortice, l'arrivo a Haven e la cattura di Jak da parte di Erol).
- Il gioco passa da Jak 1 alla scena d'apertura di Jak 2 dopo il finale di Jak 1 (la porta
  Precursor della Cittadella), con una dissolvenza.
- Opzionale: un breve tratto giocabile prima della scena, per arrivare al Rift Rider e attivarlo.

### 4.2 Fine Jak 2 → inizio Jak 3 (livello nuovo)

Jak 3 non mostra in gioco come Jak finisce esiliato: lo racconta soltanto. Va ricreato.

- **Dove**: Haven in guerra, un anno dopo Jak 2. Si costruisce **nel motore di Jak 3**, che ha già
  tutto il necessario: i quartieri di Haven, rovine e incendi, robot della KG, Metal Head, le
  guardie della Freedom League.
- **Cosa succede**:
  1. una missione giocabile breve: difendere un quartiere, con tutto che crolla attorno;
  2. Jak viene circondato dalle guardie, disarmato e arrestato, in una scena scriptata in-engine;
  3. il processo, con il Conte Veger;
  4. dissolvenza sull'apertura di Jak 3, già pronta (`goal_src/jak3/levels/intro`: Jak
     abbandonato nel deserto).
- **Come**:
  - un nuovo task di missione GOAL e un livello custom (`build_level`, layout in Blender come per
    il mondo aperto);
  - la camera su spline, come nelle scene esistenti;
  - per l'arresto, le animazioni esistenti di Jak e delle guardie (colpito, a terra,
    camminata scortata), con dissolvenze nei punti scoperti;
  - i dialoghi con sottotitoli e battute già presenti nei giochi. **Non** si clonano le voci
    degli attori originali.
- È la parte più creativa e più lunga. Prima conviene fare un prototipo "solo camera": la scena
  dell'arresto con i personaggi fermi, per decidere inquadrature e durata.

## Fase 5 — Rifinitura

- Continuità: Daxter, statistiche cumulative, schermata finale della trilogia con il tempo totale.
- Prestazioni sui tre giochi (obiettivo 60 fps a Epic sul portatile).
- Trailer e video della trilogia.

## Prossimi passi concreti

1. Prototipo del cambio gioco a caldo Jak 2 → Jak 3 (fase 1.6).
2. In parallelo: `EnhancedPost` su Jak 2 (fase 2), a partire da Haven di notte con la pioggia.
