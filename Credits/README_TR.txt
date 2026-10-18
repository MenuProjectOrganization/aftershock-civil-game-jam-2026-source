AFTERSHOCK — Jüri kullanım notu / README şablonu

BU BELGE BİR DERLEME VEYA OYUN TESTİ BAŞARI RAPORU DEĞİLDİR.
Kontroller mevcut C++ kodundan okunmuştur; son pakette input, kayıt/ayar kalıcılığı ve farklı bilgisayar kabulü bu metinle doğrulanmış sayılmaz.

BAŞLATMA
- Windows x64 bilgisayarda paketin tamamını çıkarın/kopyalayın. Yalnız EXE’yi ayırmayın; Engine, JamProject ve Content/Paks alt ağacı birlikte kalmalıdır.
- Paket kökündeki AFTERSHOCK.exe dosyasını açın. Unreal Editor/Python veya projeyi editörde açmak son oyuncu akışı değildir.
- Hedef grafik yolu D3D12/SM6’dır; uyumlu Windows, ekran kartı ve sürücü gerekir. Bu şablon her bilgisayar için performans garantisi vermez.
- Varsa Kurulum klasöründeki resmî Microsoft runtime kurucuları isteğe bağlı önkoşullardır. Tam paket ayrıca app-local DLL’leri taşıyabilir; sistem güvenlik ayarını değiştirmek bu talimatın parçası değildir.
- İnternet bağlantısı bir oyun asset indirme adımı değildir. Credits içindeki web adresleri kaynak/lisans referanslarıdır.

İLK OTURUM
Ana menüden YENİ OYUN’u seçin; mod ve bölge/odak seçimlerini menüde yapın. DEVAM ET, açık sağlıklı oturuma döner; açık oturum yoksa en yeni sağlam Manual/Auto0/Auto1/Auto2 kaydını seçer. Uygun kayıt yoksa pasif olabilir.

VARSAYILAN KONTROLLER
W/A/S/D        Kamera taşıma
Q / E          Kamerayı sola / sağa döndürme
Fare tekeri    Yakınlaştırma / uzaklaştırma
G              Şehir / dünya görünümü
Sol tık        Bina seçme, etkin araç işlemi veya önizleme yerleştirme
Sağ tık        İnşaat önizlemesini / harita aracını iptal
R              İnşaat önizlemesini 90 derece döndür
F              İnşaat önizlemesinde temel kalitesini değiştir
PageUp/PageDown Önizlemenin kat sayısını artır / azalt; bina sınırları geçerlidir
Esc            Açık panelden geri dön; araç/önizlemeyi iptal et; sonra duraklat/devam et

Ok tuşları varsayılan WASD bağları korunuyorsa ve ok başka işleme atanmadıysa kamera alias’ıdır. Bütün tuşlar her menü/araç durumunda aynı eylemi yapmaz.

AYARLAR
Grafik, Ses, Kontroller, Oynanış ve Erişilebilirlik sayfaları vardır. Kontroller’den 11 klavye eylemi yeniden atanabilir; kamera hassasiyeti, ters teker ve yumuşak kamera seçenekleri vardır. Sol/sağ fare, teker ve Esc sabittir. UYGULA değişikliği etkinleştirir. Ekran değişiklikleri için 15 saniyelik onay/geri alma akışı bulunur. Erişilemeyen render seçenekleri menüde açıklamalı kapalı olabilir.

KAYDETME VE GÜVENLİ ÇIKIŞ
- Esc → ŞEHRİ KAYDET: Manual yuvasına kayıt isteği gönderir; sonuç kartını okuyun.
- Kaydederek çıkma yolu: Esc → ANA MENÜ / OTURUMU KORU → OYUNDAN ÇIK.
- Açılış menüsündeki OYUNDAN ÇIK, sağlıklı canlı oturum varsa önce Manual kaydı alır; kayıt başarısızsa çıkış yapmadan hata gösterir. UI’da ayrıca “Save & Exit” adlı düğme olduğu varsayılmaz.
- Yeni Oyun, mevcut sağlıklı açık şehri önce aynı Manual yuvasına kaydeder. Kayıt başarısızsa geçiş durur. Her şehre ayrı dosya/arşiv garantisi yoktur; Manual ortak yuvadır.
- Kaynak kodda tur ilerleme/afet sonrası hazır evre ve aktif oturumda 120 saniyelik sayaç için otomatik kayıt tetikleri bulunur. Üç dönen otomatik yuva vardır. Bu not, çalışan pakette tüm kayıt yollarının test edildiği iddiası değildir.

KOPYALAMA VE KAYNAKLAR
Bu metin, CREDITS.md, licenses, notices, provenance, inventory.json ve SHA256SUMS.txt birlikte korunmalıdır. Kaynak medya/uasset, kullanıcı INI’si, kişisel kayıt, hesap anahtarı veya ham UE log’u bu belge paketine dahil değildir.

Hak/provenans açıkları LISANS_BOSLUKLARI_TR.txt içinde açıkça belirtilmiştir. Bu küçük atıf paketi üçüncü tarafların bütün dağıtım şartlarını veya bağımsız hak kontrolünü ikame etmez.
