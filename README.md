# Flexit UNI3 – ESPHome-panel over Modbus

Et ESP32-S3-kort som erstatter (eller sitter ved siden av) CI600-veggpanelet på et Flexit-aggregat med
CS60-styring. Du får all styring og alle målinger inn i Home Assistant, og kan legge på automatikk etter behov.

*English: ESPHome replacement for the Flexit CI600 wall panel (CS60 controller, e.g. UNI3) over RS485/Modbus.
Docs are in Norwegian; the YAML is easy to follow with a translator.*

![RS485-modulen koblet til kabelen fra CS60](docs/bilder/rs485-kobling.jpg)

## Hva du får

- **I Home Assistant:** modus (Stop/Min/Normal/Max), temperatur-settpunkt og viftehastighet (%) for tilluft
  og avtrekk i hver modus. Du får også ute- og tillufttemperatur, viftehastighet, varmegjenvinning,
  varmebatteri og filteralarm, og knapper for å nullstille alarmer.
- **Valgfri skjerm:** en 1,5" OLED som viser drift, temperaturer og vifter.
- **Valgfri automatikk**, som du slår av og på med én linje per pakke:
  - stopp viftene ved røyk
  - vedfyring (lett overtrykk)
  - boost
  - mer luft ved dusj eller høy CO2
  - mindre luft ved regn eller tørr inneluft
  - duggpunkt i aggregatet

**Testet på:** Flexit UNI3 med CS60-styring, med CI600-panelet fjernet. Det skal også virke på andre aggregater
med CS60 og CI600-panel.

## Slik virker det

CS60 er «sjef» (Modbus-master) på bussen til veggpanelet. Omtrent 9 ganger i sekundet sender den hele
tilstanden sin til alle som lytter, og den spør panelene om de har noe nytt. ESP-en later som den er et
CI600-panel på **adresse 3** (originalpanelet bruker 2). Den leser det CS60 sender, og legger kommandoer klare
som CS60 henter.

> **Viktig:** CS60 leter bare etter paneler (adresse 1–3) i det den starter. **ESP-en må være på før CS60.**
> Får ESP-en strøm fra samme kabel som i koblingen under, ordner dette seg som regel selv. Ellers: skru av
> aggregatet, vent litt, og skru på igjen når ESP-en er oppe.

## Deler

| Del | Søkeord / merknad |
|---|---|
| ESP32-S3 Super Mini | «ESP32-S3 Super Mini». Andre ESP32-S3-kort går også, men da må du sjekke pinnene. |
| TTL-RS485-modul | Helst en **isolert** modul med **automatisk retning** («TTL to RS485 isolated module», med B0505-DC-DC-omformer på kortet). En MAX485/MAX3485-modul med DE/RE-pinne går også, se [DE/RE-pinne](#dere-pinne). |
| Buck-omformer 12 V → 5 V | En liten «mini buck step-down» som tåler 12 V inn. |
| OLED 1,5" 128x128 SH1107 (I2C) | Valgfri. |
| RJ12-kabel (6 ledere) | Fra CS60-kortet. Du kan bruke kabelen som gikk til det originale panelet. |

## Kobling

![Koblingsskjema](docs/kobling.svg)

**RJ12 fra CS60** (pinne 1 → 6):

| Pinne | Farge | Signal | Kobles til |
|---|---|---|---|
| 1 | Brun | +12 V | Buck IN+ |
| 2 | Gul | +12 V | **ikke i bruk** (isoleres) |
| 3 | Grønn | B | RS485 B+ |
| 4 | Rød | A | RS485 A+ |
| 5 | Svart | GND | RS485 ⏚ (buss-GND) |
| 6 | Blå | GND | Buck IN− |

**Resten av koblingen:**

| Fra | Til |
|---|---|
| Buck OUT+ (5 V) | ESP 5V og RS485 VCC |
| Buck OUT− | ESP GND og RS485 GND |
| RS485 TX | ESP **GPIO44** (RX) |
| RS485 RX | ESP **GPIO43** (TX) |
| OLED VCC / GND | ESP 3V3 / GND |
| OLED SDA / SCL | ESP **GPIO4** / **GPIO3** |

**Om bildet øverst:**
- Det sorte kortet er den isolerte RS485-modulen. Den er limt oppå det blå buck-kortet.
- Nederst kommer RJ12-kabelen inn: svart til ⏚, grønn til B+ og rød til A+ på RS485-modulen. Brun (+12 V) og
  blå (GND) går til buck-kortet under.
- Øverst går fire ledninger til ESP-en: 5 V, GND, TX og RX.
- Modulen styrer sende/motta-retningen selv, så den har ingen DE-ledning.

> **Spenning:**
> - Gul ledning er også +12 V. Den må aldri kobles til ESP-en.
> - Signalnivået på RS485-modulens TX følger som regel modulens VCC, mens ESP32-S3 er laget for 3,3 V på
>   pinnene. Står det «3.3V/5V» på modulen, er det tryggest å koble VCC på modulen til **3V3** på ESP-en i
>   stedet for 5 V.

## Programvare

Du trenger Home Assistant med **ESPHome Device Builder** (tillegget «ESPHome»), versjon 2026.9 eller nyere.

1. Åpne ESPHome Device Builder, trykk **+ New device** og gi den navnet `flexit-uni3`.
2. Trykk **Edit** på den nye enheten, slett innholdet og lim inn hele [`flexit-uni3.yaml`](flexit-uni3.yaml).
3. Åpne **Secrets** (øverst til høyre) og legg til `flexit_api_key`. Se
   [`secrets.yaml.example`](secrets.yaml.example). Nøkkelen kan du lage med `openssl rand -base64 32`, eller
   kopiere den Device Builder laget for den nye enheten. Den samme nøkkelen brukes også for OTA.
4. Velg hvilke pakker du vil ha med under `packages:` ved å fjerne `#` foran dem, og rett entitets-ID-ene under
   `substitutions:`. Se [Pakker](#pakker).
5. Koble ESP-en til PC-en med USB, og velg **Install → Plug into this computer** første gang. Senere
   oppdateringer går over WiFi.
6. Home Assistant finner enheten selv (Innstillinger → Enheter og tjenester). Trykk **Konfigurer** og lim inn
   API-nøkkelen. Pakkene som bruker sensorer fra HA (røyk, dusj, CO2 osv.) får verdiene først når enheten er
   lagt til her.

Koden hentes fra dette repoet (`github://galvheim/esphome-flexit-uni3@main`) og oppdateres når du kompilerer.
Vil du heller ha en fast versjon, bytter du `main` med en commit-hash eller tag. Vil du jobbe med en lokal kopi,
legger du `components/` og `packages/` i ESPHome-mappen og bruker `type: local` og `!include` i stedet.

### Første oppstart

1. Monter ESP-en, slå av aggregatet, og slå det på igjen. ESP-en starter da før CS60.
2. Åpne loggen i ESPHome (**Logs → Wirelessly**). Hvert minutt kommer en linje som denne:
   ```
   Til adresse 3: oppstartsskann(04)=4 coil-poll(01)=812 les(03)=0 annet=0 | broadcast fra CS60=540
   ```
   - `broadcast` øker: data fra CS60 kommer fram.
   - `coil-poll` øker: CS60 har godtatt ESP-en som panel, og kommandoer virker.
3. Ved første oppstart **overtar ESP-en viftehastighetene anlegget allerede bruker**. Etter det er det
   ESP-en som eier dem, og du endrer dem i HA.

## Pakker

`kjerne.yaml` må alltid være med. De andre er valgfrie og avhenger ikke av hverandre.

| Pakke | Gjør | Trenger (substitution) |
|---|---|---|
| `kjerne.yaml` | Panelet: målinger, modus, settpunkt, vifter, alarmer, bryteren «Automatikk» | – |
| `skjerm.yaml` | OLED 128x128 med to sider som veksler | `i2c_sda_pin`, `i2c_scl_pin` |
| `roykstopp.yaml` | Stop når røykvarsler går, og tilbake når den er klar. Bryter «Røykstopp». | `royk_entitet` |
| `peis.yaml` | Bryter «Vedfyring»: Normal og senket avtrekk (lett overtrykk) i `peis_min` min | – |
| `boost.yaml` | Bryter «Boost»: Max i `boost_min` min | – |
| `dusj.yaml` | Minst `dusj_modus` når fukten på badet stiger raskt | `bad_fukt_entitet` |
| `co2.yaml` | Minst Normal over `co2_hoy_ppm` | `co2_entitet` |
| `torr_luft.yaml` | Høyst Min når inneluften er under `torr_inn_rh` % | `inne_fukt_entitet` |
| `regn.yaml` | Høyst Min ved regn, yr eller tåke i værvarselet | `vaer_entitet` |
| `kondens.yaml` | Duggpunkt og duggpunktsmargin inne i aggregatet (bare visning) | `aggregat_temp_entitet`, `aggregat_fukt_entitet` |

Alle innstillinger og standardverdier står øverst i hver pakkefil under `substitutions:`. Du overstyrer dem
ved å skrive samme navn i `substitutions:` i din egen `flexit-uni3.yaml`.

**Flere sensorer av samme slag?**
- *Flere røykvarslere:* lag en **Gruppe**-hjelper i HA (Binær sensor-gruppe) og bruk den.
- *Fukt fra flere rom:* lag en hjelper som **kombinerer tilstanden** til flere sensorer (gjennomsnitt) og bruk den.

### Prioritet i automatikken

Hvert minutt velges modus i denne rekkefølgen:

1. **Røyk** → Stop. Gjelder alltid.
2. **Vedfyring** → Normal. Gjelder alltid.
3. **Boost** → Max. Gjelder alltid.
4. Er **Automatikk** på, brukes *grunnmodus* (det du sist valgte i «Sett modus») som utgangspunkt:
   - tørr luft eller regn → høyst Min
   - høy CO2 → minst Normal
   - dusj → minst `dusj_modus`

Automatikken setter aldri Stop av seg selv. Er Automatikk av, styrer du modus selv. Røyk, vedfyring og boost
virker likevel, og går tilbake til forrige modus når de er ferdige.

Sensoren **Automatikk status** viser hva som styrer akkurat nå.

## Home Assistant

Med enhetsnavnet «Flexit UNI3» får du blant annet:

| Entitet | Hva |
|---|---|
| `select.flexit_uni3_sett_modus` | Velg Stop/Min/Normal/Max |
| `number.flexit_uni3_temperatur_innstilling` | Settpunkt for tilluft |
| `number.flexit_uni3_tilluft_min` … `_avtrekk_maks` | Viftehastighet (%) per modus |
| `sensor.flexit_uni3_tilluft_temperatur`, `_uteluft_temperatur` | Temperaturer |
| `sensor.flexit_uni3_tilluft_viftehastighet` | Vifte nå (%) |
| `sensor.flexit_uni3_varmegjenvinning`, `_varmebatteri` | Gjenvinning og varmebatteri (%) |
| `binary_sensor.flexit_uni3_filterbytte` | Filteralarm |
| `switch.flexit_uni3_automatikk` | Automatikk av/på |
| `sensor.flexit_uni3_automatikk_status` | Hva som styrer nå |

Et ferdig dashboard (bare innebygde kort) finner du i
[`ha-eksempler/dashboard-ventilasjon.yaml`](ha-eksempler/dashboard-ventilasjon.yaml).

## Feilsøking

| Symptom | Prøv |
|---|---|
| `broadcast fra CS60=0` | Bytt A og B. Sjekk at buss-GND (svart) er koblet. Bytt TX og RX mellom ESP og modul. |
| `broadcast` øker, men `coil-poll=0` | CS60 har ikke funnet ESP-en. Slå aggregatet av og på mens ESP-en er på. Sjekk at `modbus_adresse` ikke er den samme som et annet panel bruker. |
| Verdiene i HA står stille | Ikke senk loggnivået under `DEBUG` (kjent feil, MSkjel issue #16). |
| Skjermen er svart | Sjekk SDA/SCL. Noen skjermer bruker adresse `0x3D` (`skjerm_adresse`). |
| Viftene går tilbake til gamle verdier | Er originalpanelet fortsatt koblet til, kan det sende sine verdier. ESP-en sender sine på nytt etter `vifte_synk_min` minutter. |

Trenger du mer detaljert logg, setter du `logger: level: VERBOSE`. Da logges hver Modbus-ramme som hex.

### DE/RE-pinne

Moduler med MAX485, MAX3485 eller lignende har en DE/RE-pinne som må styres for at ESP-en skal kunne sende.
Koble DE og RE sammen til **GPIO8** (gjerne med 10 kΩ ned til GND), og fjern `#` foran blokken nederst i
`flexit-uni3.yaml`:

```yaml
flexit_modbus_server:
  - id: !extend server
    tx_enable_pin: GPIO8
```

Moduler med automatisk retning (som den på bildet) trenger ikke dette.

### Sammen med originalpanelet

ESP-en bruker adresse 3, så CI600-panelet (adresse 2) kan i prinsippet sitte på samme buss. Det er slik
MSkjel sin opprinnelige komponent brukes, men dette oppsettet er bare testet med panelet fjernet. Med begge
koblet til kan begge endre innstillinger, og ESP-en sender sine viftehastigheter på nytt når CS60 melder noe
annet. Ifølge MSkjel kan tilluftstemperaturen bare settes fra ESP-en når CI600 ikke er koblet til.

## Takk og lisens

- Modbus-komponenten bygger på [MSkjel/esphome-flexit-modbus-server](https://github.com/MSkjel/esphome-flexit-modbus-server).
  Biblioteket under er [MSkjel/ESP-ModbusRTUServer](https://github.com/MSkjel/ESP-ModbusRTUServer).
  Begge har MIT-lisens. Endringene i denne kopien står øverst i
  [`flexit_modbus_server.cpp`](components/flexit_modbus_server/flexit_modbus_server.cpp).
- Lisens: [MIT](LICENSE).
- Dette er et hobbyprosjekt uten tilknytning til Flexit. Bruk på eget ansvar. Røykstopp-funksjonen er ikke et
  godkjent brannsikringssystem.
