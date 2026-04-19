Začal som skumaním repozitára Envoy, kde mi dosť pomohla AI zorientovať sa v jednotlivých konceptoch. Keďže som s ním ešte nikdy nepracoval. Preskúmal som hlavne časť s http filtrami, keďže hlavne o tom bol môj task. Následne som si ho chcel zbuildiť, ale potom som zistil že používa na build tool Bazel, o ktorom som taktiež nič nevedel takže som si musel overiť ako funguje. Keď som sa zorientoval spustil som build, ktorý trval cca 2 hodiny. Následne som si vytvoril jednoduchý .yaml config file aby som si skúsil spustiť tento proxy server. Pozostával z jedoduchého presmerovania na stránku example.com.

Implementáciu som začal nejlehčím taskom a to implementovať kruhovu frontu pre strukturu CacheResponse,(ring_buffer.h).
Následne som si vytvoril triedu Cache, ktorú som chcel použit na uloženie HTTP odpovedí, pozostáva z hash mapy pre rýchle vkladanie a vyhľadávanie (O(1)). Klíč bol určený v zadaní a value bol samotný buffer. Ďalej boli implementované 2 simple metódy lookup a insert. Zároveň je v tejto classe použitý lock guard, aby sa predišlo prípadným race conditions pri používaní viacero workerov.
Potom som prešiel na samotnú implemenáciu filtru, kde som si vytvoril triedu SimpleCacheFilter, kde som deklaroval setter na callbacky, decode/encode header, data a trailer, build cache key, metody na odoslanie odpovede či už cashovanej alebo nie. Implementacia decode/encode spočívala v tom že ak je odpoved uložená v mape Cache vrátila sa cez métodu replayCachedResponse, ak nie tak sa odoslala a jej odpoved sa uložila do mapy pre ďalšie rovnaké requesty. Následne som vytvoril config na vytvorenie faktorky na vytvorenie samotného filtru. Pridal subory do Bazel BULID a upravil config na konfigurovatelnú veľkosť bufferu cez .yaml config.

Následne som si chcel moju implementáciu otestovat, tak som si cez AI vytvoril jednoduchý server pomocou pythonu, .yaml config kde som zahrnul môj filter a shellovský script na posielanie requestov cez curl. V tomto teste sa ukazalo že moja implementace má značnú chybu a to že sa nekontrolovalo pri viacerých workeroch či uz nejaký na danom requeste pracuje tj. poslal som ť rovnakých requestov každý dostal 1 worker a nezávysle na sebe 5x poslali request aj keď bol rovnaký.

Tento problém mal v skutočnosti dve príčiny. Prvá bola race condition — všetky workery čítali upstream_in_flight == false súčasne, ešte predtým ako ktorýkoľvek stihol zapísať true. Druhá bola, že aj keby sa race condition vyriešil, priame volanie waiter_cb->encodeHeaders() z iného worker threadu je v Envoy nesprávne — každá connection smie byť dotknutá len z threadu ktorý ju vlastní.

Riešením bolo pridanie triedy CoalesceMap, ktorá obaluje mapu vlastným mutexom. Metóda tryBecomePrimary() robí celú operáciu čítaj-skontroluj-zapis atomicky, takže presne jeden worker sa stane primárnym a ostatní sa zaregistrujú ako waiteri. Namiesto priameho volania callbacku sa používa dispatcher->post(), čo je Envoyov thread-safe mechanizmus na odoslanie práce na event loop iného threadu. Waiter teda dostane odpoveď priamo streamovanú od primárneho workera hneď ako príde — nie až po uložení do cache. To je dôležité pretože zabráni vypršaniu idle timeoutov.

Ako by sa to dalo riešiť inak

Alternatívou by bolo ponechať koalescenciu čisto per-worker — každý worker má vlastnú inštanciu CoalesceMap bez akéhokoľvek mutexu. Výhodou je nulová synchronizácia a žiadne cross-thread dispatche, nevýhodou je že thundering herd sa rieši len v rámci jedného workera, nie globálne. Pri výraznom zaťažení to stále výrazne pomôže, len nie tak dokonalo. 

Kde riešenie nie je optimálne a čo by šlo zlepšiť

Cache rastie neobmedzene — počet ring bufferov nie je limitovaný. Pri tisíckach unikátnych URL je to v praxi memory leak. Mutex na CoalesceMap a Cache je globálny, čo pri vysokom počte concurrent requestov na rôzne kľúče môže byť bottleneck.

Z bezpečnostného hľadiska je problém to, že filter cachuje odpovede bez ohľadu na Authorization header. Dvaja rôzni používatelia s rovnakou URL by dostali tú istú odpoveď, čo je závažná bezpečnostná diera.

Každý encodeData chunk volá getWaiters() čo zamkne mutex — pri streamovaných odpovediach s mnohými chunkmi to môže byť znateľné. Optimalizácia by bola cachovať snapshot waiterov už pri encodeHeaders a použiť ho pre všetky následné chunky.

Watermarking

V kontexte koalescencie watermarking nefunguje správne pretože primárny request riadi flow control sám za seba, ale waiteri sú pasívni príjemcovia ktorí dostávajú dáta cez dispatcher->post() bez akejkoľvek spätnej väzby. Ak je niektorý waiter pomalý, jeho buffer sa plní ale primárny request o tom nevie a pokračuje v prijímaní dát z upstream plnou rýchlosťou.

Spoločná zmena pre všetky HTTP verzie by bola: CoalesceMap by musela uchovávať nielen zoznam waiterov ale aj ich aktuálny backpressure stav. Primárny request by pred každým encodeData musel skontrolovať či všetci waiteri sú pripravení prijať dáta, a ak nie, zavolať onDecoderFilterAboveWriteBufferHighWatermark() na sebe samom aby zastavil upstream. Toto by ale degradovalo výkon na rýchlosť najpomalšieho klienta.

Časová náročnosť

Research Envoy + Bazel: ~10 hodín
Implementácia ring buffer + Cache + filter: ~10 hodín
Debug koalescencie (race condition + cross-thread dispatch): ~2 hodiny
