# AFTERSHOCK — Jüri paketi kaynak ve atıfları

Bu belge mevcut proje kredi/lisans/provenans kayıtlarından hazırlanmıştır. Bir build, oyun testi veya bütün içerik için hak temizliği sertifikası değildir. Kaynak dosya hash’leri `inventory.json` içindedir. Takım üretimi, CC0 içerik ve lisanslı içerik aynı lisans altında gösterilmez.

## Proje ve motor

AFTERSHOCK / METU NCC CYBER SECURITY STUDIOS. Oyun kodu, prosedürel vatandaş/ulaşım/ikon/hasar/su/yangın sunumları ve belirtilen sentez sesleri proje üretimidir. Bu ifade üçüncü taraf veriyi veya ses/model lisansını devralmaz; genel CC0/MIT izni vermez.

Unreal Engine — Epic Games, ilgili Unreal Engine EULA’sı kapsamında. Motor/üçüncü taraf runtime bildirileri ve Microsoft/DirectX dağıtım kayıtları paket içindeki kendi şartlarını korur. Bu küçük belge seçkisi tüm motor/OS lisanslarının yerine geçmez.

## Modeller, yüzeyler ve ses

- Kenney: Starter Kit City Builder modelleri/sesleri ile Platformer ve FPS sesleri; mevcut kaynak kayıtlarında sanat/ses için CC0 1.0. Upstream şablon kodunun MIT lisansı ayrıdır. Tam MIT metinleri `licenses/`, sanat beyanı `notices/Kenney-Art-CC0-Statement.txt` içindedir.
- Poly Haven — Powered by Poly Haven. Leafy Grass: Charlotte Baglioni; Painted Plaster Wall/Clay Roof Tiles 02/Wood Planks/Dark Rock 02: Amal Kumar; Dry Ground 01/Asphalt 02: Rob Tuytel. Diğer bölgesel yüzeyler ve tam kaynak adları provenance/notices dosyalarında kayıtlıdır. Lisans: ilgili kaynaklarda CC0 1.0, https://polyhaven.com/license .
- ambientCG Grass001 — CC0 1.0, https://ambientcg.com/a/Grass001 ve https://docs.ambientcg.com/license/ . Bu bir prosedürel PBR kaynağıdır; taranmış yerel arazi olduğu iddia edilmez.
- Rain (loopable): Ylmir; Water Waves: transitking; Mild Wind Background Noise: Bashar from Skirmish.io. OpenGameArt kaynakları, CC0 1.0. URL ve işleme ayrımları `notices/Weather-Audio.txt` ve `notices/OpenGameArt-Audio.txt` içindedir.
- City Sample 5.8 — Epic Games. Seçilmiş çevre modelleri ve mimari yüzeyler AFTERSHOCK’a uyarlanmıştır. Bu içerik CC0/açık kaynak değildir. İlgili edinim şartlarına tabi cooked oyun içeriğidir; raw asset paketi dağıtılmaz. https://www.fab.com/listings/4898e707-7855-404b-af0e-a505ee690e68 , https://www.unrealengine.com/eula/content , https://www.fab.com/eula .
- ElevenLabs text_to_sound_v2 ile üretilmiş efektler için mevcut üretim manifesti vardır. İncelenen manifest plan/dağıtım lisansı içermez. Hak kapsamı ayrı teyit gerektirir; bu seslere CC0 veya sınırsız yeniden kullanım izni atanmaz.

## Harita, yükselti, iklim ve tarihsel görüntü

- © OpenStreetMap contributors. ODbL 1.0: https://www.openstreetmap.org/copyright ve https://opendatacommons.org/licenses/odbl/1-0/ . Tam mevcut lisans kopyası `licenses/OSM-ODbL-1.0.html` içindedir. Maskeler/tablolar türetilmiştir; oyun şehri gerçek binaların kadastro rekonstrüksiyonu değildir. Kaynak/provenans ve veri paylaşımı yükümlülükleri ayrıca korunmalıdır.
- Made with Natural Earth. Natural Earth harita verileri public domain: https://www.naturalearthdata.com/about/terms-of-use/ . Ölçek genelleştirmesi, rasterleştirme ve kırpma yapılmıştır.
- Contains modified Copernicus DEM data (2021), supplied by Open-Meteo. DOI:10.5270/ESA-c5d3d65 . Open-Meteo API veri kaydı CC BY 4.0: https://open-meteo.com/en/licence ve https://creativecommons.org/licenses/by/4.0/ . Seyrek örnekleme/interpolasyon uygulanmıştır.
- Güncel Copernicus WorldDEM-30 veri bildirimi:

  produced using Copernicus WorldDEM-30 © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018 provided under COPERNICUS by the European Union and ESA; all rights reserved

  Copernicus verisi ücretsiz kullanım şartlarına tabidir; CC0/public domain diye etiketlenmez. https://dataspace.copernicus.eu/explore-data/data-collections/copernicus-contributing-missions/collections-description/COP-DEM . Örnekleme, kırpma ve enterpolasyon yapılmıştır.
- Mapzen/Tilezen Terrarium mozaiği: kaynak veri kümelerinin kendi şartları geçerlidir. Mevcut tam sağlayıcı atıf listesi `licenses/Mapzen-Terrain-Attribution.md` içindedir; listedeki bütün kaynakların her bölgede kullanıldığı iddia edilmez. USGS 3DEP/GMTED2010/SRTM verileri için kaynak US Geological Survey’dir; ilgili USGS veri açıklaması proje kaynak kayıtlarında bulunur.
- NASA POWER/MERRA2 aylık iklim verileri. Kaynak kayıtlarında 2001–2025 dönemi ve kaba grid ortalamaları belirtilir; yerel istasyon veya afet tahmini değildir. NASA acknowledgement/citations: https://power.larc.nasa.gov/docs/methodology/citations/ .
- EOxCloudless https://cloudless.eox.at by EOX IT Services GmbH (Contains modified Copernicus Sentinel data 2016 & 2017)

  Kullanılan tarihsel EOxCloudless2016 kaynağı için mevcut sağlayıcı kaydı CC BY 4.0 açık istisnasını belirtir; daha yeni NC ürünlere bu izin aktarılmaz. https://creativecommons.org/licenses/by/4.0/ . Kırpma/örnekleme, iki yarıkürenin birleştirilmesi ve JPEG yeniden kodlama, renk katmanı karışımı ve Unreal mip üretimi kaynak kayıtlarında belirtilen değişikliklerdir. Sağlayıcı desteği/endorsement veya güncel bulutsuz uydu görüntüsü iddia edilmez. Yerel policy kopyası ve hash’i `licenses/EOX-2016-Provider-Policy.html`, `provenance/EOX-2016-License-Verification.json` içindedir.

## Açılış, logo ve font

Kullanıcının sağladığı açılış resmi ve sahibinin sağladığı METU NCC/CYBER SECURITY logo düzenlemesi proje kullanım/düzenleme yetkisine dayanır. Bunlara genel CC0, marka devri veya herkese açık yeniden kullanım izni atanmaz. Kaynak kayıtları `provenance/Opening-Artwork.json` ve `notices/Studio-Branding-Source.md` içindedir.

Logo alt yazısında Bitstream Vera Sans Regular’den değiştirilmemiş font glifleri outline olarak kullanılmıştır. Copyright © 2003 Bitstream, Inc.; tam copyright/permission notice `licenses/Bitstream-Vera.txt` içindedir. Bu belge paketinde font dosyası veya ham görsel/ses/model yoktur.

Geliştirme araçları Blender/Python/Pillow/NumPy vb. bu belge paketiyle dağıtılmaz. Mevcut proje kayıtları onları çevrimdışı üretim araçları olarak ayırır; oyun runtime’ına aktarılmış lisans hakkı varsayılmaz.
