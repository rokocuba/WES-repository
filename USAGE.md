# usage

ovo je usage readme za nas wes sustav.
pisano je kratko i direktno, da mozes odmah sloziti demo.

## sto je sto
- esp32 + ekran (ovaj repo): glavni uredaj s touch ui-em.
- esp32-s3 camera node: salje jpeg frame na zahtjev.
- esp32-s3 ml node: lokalno vrti model za prepoznavanje znamenki.
- esp32-s3 anchor node: beacon/ap za distance alarm.
- web sucelje: odvojeno, koristi se za glazbu i kontrolu.

## sto radi sustav
- touch ui sa 4 ekrana: home, music, number, vlaga.
- hvata sliku s camera noda i prikazuje preview.
- salje sliku na ml node i vraca predikciju 0-9.
- pusta audio preko i2s (wav iz spiffs-a).
- cita vlagu i temperaturu sa sht31 senzora.
- pali beep/sirenu kad je sustav predaleko od anchor noda.

## web esp32-s3 node (odvojeni firmware)
- spaja se na lokalni websocket server.
- prima komande i vraca json rezultat.
- salje healthcheck status.
- moze slati dummy/capture image.
- moze primati audio stream komande za playback.

komande koje koristimo:
- healthcheck
- capture_image
- audio_start
- audio_data
- audio_stop

## ui flow

### home scr

![home scr](assets/IMG-20260329-WA0009.jpg)

- tipka `123` vodi na number ekran.
- tipka s notom vodi na music ekran.
- tipka s tri tocke vodi na vlaga ekran.

### music scr

![music scr](assets/IMG-20260329-WA0010.jpg)

- centralna tipka (pause ikona) triggera audio playback (`/spiffs/output.wav`).
- home tipka vraca na home.
- ekran je za lokalnu reprodukciju glazbe/sounda iz uredaja.

### number scr

![number scr](assets/IMG-20260329-WA0011.jpg)

- tipka kamere okida capture (slikaj_sliku).
- gore lijevo je preview zadnje jpeg slike.
- velika oznaka dolje prikazuje prepoznatu znamenku (0-9).
- home tipka vraca na home.

### vlaga scr

![vlaga scr](assets/IMG-20260329-WA0012.jpg)

- prikazuje `vlaga` i `temperatura` u realnom vremenu.
- vrijednosti se osvjezavaju iz sht31 servisa.
- home tipka vraca na home.

## digit model (glavna znacajka)
- model radi lokalno na esp32-s3 ml nodu (embedded inferencija, bez clouda).
- ulaz je jpeg frame, izlaz je `RESULT <digit> <confidence>`.
- pipeline je optimiran za znamenke: resize na 28x28, adaptivni threshold, ciscenje maske, invert i centriranje.
- inferencija je preko tflm-a (digit 0-9 + confidence + class scoreovi).
- bitno: build ml noda je bez python koraka u normalnom build flowu (koriste se vec generirani static c asseti).

## end-to-end flow za broj
1. na number ekranu stisnes kameru.
2. esp32 ekran uredaj salje `SEND_PIC` camera nodu preko uart1.
3. dobije jpeg, prikaze preview na ekranu.
4. isti jpeg salje ml nodu preko uart2:
   - `INFER_JPEG`
   - `<jpeg_size>`
   - `<jpeg_bytes>`
5. ml node vraca `RESULT <digit> <confidence>`.
6. ui labela na number ekranu se updatea na predikciju.

## distance alarm (anchor node)
- glavni uredaj periodicki skenira ssid `FTM_Anchor`.
- iz rssi procjenjuje udaljenost.
- ako je procjena veca od limita (`6.0 m`), pali se sirena/beep.
- beep ima cooldown (da ne trigerira svake sekunde).
- sirena se generira lokalno i pusta preko i2s izlaza.

## bitne konfiguracije (ovaj repo)
- audio i2s pinovi su u `main/app_main.c`:
  - bclk = gpio25
  - lrc/ws = gpio33
  - dout = gpio32
- camera node uart je u `main/camera_capture.c`:
  - tx = gpio27
  - rx = gpio26
- ml node uart je u `main/app_main.c`:
  - tx = gpio14
  - rx = gpio2
- sht31 i2c je u `main/sht31_service.c`:
  - sda = gpio22
  - scl = gpio21

## prije builda
1. provjeri da je `spiffs_data/output.wav` prisutan (za music ekran).
2. spoji uart linije prema camera i ml nodu.
3. spoji i2s izlaz na pojacalo/zvucnik.
4. spoji sht31 na i2c pinove.

## build i flash (ekran uredaj, ovaj repo)
- `idf.py set-target esp32`
- `idf.py build`
- `idf.py -p COMx flash monitor`

## build i flash (ml node i anchor node)
- ta dva firmware-a su odvojena od ovog repoa.
- za oba je target esp32s3.
- tipicno:
  - `idf.py set-target esp32s3`
  - `idf.py build`
  - `idf.py -p COMx flash monitor`

## napomena
- web sucelje je odvojeno.
- ovaj repo je device side (ekran uredaj + lokalna orkestracija flowa).
