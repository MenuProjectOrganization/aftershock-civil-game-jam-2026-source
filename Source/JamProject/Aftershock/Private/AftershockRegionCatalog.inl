// GENERATED from REGION_CATALOG.json; authored policy, real DEM cached separately.
static const FAftershockGeographicLocation GeographicFocuses[] = {
	{ TEXT("atakent"), TEXT("İstanbul / Atakent"), 41.041, 28.775, 0.045, 0.06, TEXT("atakent"), TEXT("Atakent yerel mahalle") },
	{ TEXT("dereyolu"), TEXT("Trabzon / Sürmene"), 40.912, 40.112, 0.045, 0.06, TEXT("dereyolu"), TEXT("Sürmene vadi odağı") },
	{ TEXT("yelova"), TEXT("Hatay / Belen"), 36.49, 36.19, 0.045, 0.06, TEXT("yelova"), TEXT("Belen geçidi") },
	{ TEXT("kiyikent"), TEXT("Hatay / Samandağ"), 36.09, 35.98, 0.045, 0.06, TEXT("kiyikent"), TEXT("Samandağ kıyı odağı") },
	{ TEXT("mugla"), TEXT("Muğla / Marmaris"), 36.855, 28.274, 0.045, 0.06, TEXT("mugla"), TEXT("Marmaris orman-kent kenarı") },
	{ TEXT("etna"), TEXT("Sicilia / Etna"), 37.73, 15.015, 0.07, 0.08, TEXT("etna"), TEXT("Etna kaynak çevresi") },
	{ TEXT("konya"), TEXT("Konya / Karapınar"), 37.715, 33.55, 0.045, 0.06, TEXT("konya"), TEXT("Karapınar tarım odağı") },
	{ TEXT("miami_keys"), TEXT("Florida / Miami–Key West"), 25.77, -80.185, 0.025, 0.035, TEXT("miami"), TEXT("Miami kıyı ve nehir ağzı") },
	{ TEXT("miami_keys"), TEXT("Florida / Miami–Key West"), 25.0865, -80.4473, 0.035, 0.04, TEXT("key_largo"), TEXT("Key Largo yerel ada odağı") },
	{ TEXT("miami_keys"), TEXT("Florida / Miami–Key West"), 24.726, -81.075, 0.025, 0.045, TEXT("marathon"), TEXT("Marathon yerel ada odağı") },
	{ TEXT("miami_keys"), TEXT("Florida / Miami–Key West"), 24.555, -81.781, 0.025, 0.035, TEXT("key_west"), TEXT("Key West yerel ada odağı") },
	{ TEXT("cyprus"), TEXT("Kıbrıs / Kalkanlı–Alayköy–Lefke"), 35.245, 33.032, 0.025, 0.035, TEXT("kalkanli"), TEXT("Kalkanlı tepe ve tarım odağı") },
	{ TEXT("cyprus"), TEXT("Kıbrıs / Kalkanlı–Alayköy–Lefke"), 35.177, 33.216, 0.025, 0.035, TEXT("alaykoy"), TEXT("Alayköy ova odağı") },
	{ TEXT("cyprus"), TEXT("Kıbrıs / Kalkanlı–Alayköy–Lefke"), 35.112, 32.85, 0.025, 0.035, TEXT("lefke"), TEXT("Lefke vadi ve tarım odağı") },
	{ TEXT("yakutsk"), TEXT("Yakutsk / Lena"), 62.035, 129.735, 0.035, 0.055, TEXT("yakutsk"), TEXT("Yakutsk–Lena kıyısı") },
	{ TEXT("valparaiso"), TEXT("Chile / Valparaíso"), -33.043, -71.617, 0.035, 0.045, TEXT("valparaiso"), TEXT("Valparaíso kıyı-yamaç") },
	{ TEXT("valdivia"), TEXT("Chile / Valdivia"), -39.817, -73.245, 0.035, 0.05, TEXT("valdivia"), TEXT("Valdivia nehir kolları") },
	{ TEXT("sendai"), TEXT("Japan / Sendai kıyısı"), 38.255, 140.99, 0.035, 0.04, TEXT("sendai_coast"), TEXT("Sendai kıyı ovası") },
	{ TEXT("rotterdam"), TEXT("Nederland / Rotterdam deltası"), 51.914, 4.48, 0.03, 0.05, TEXT("rotterdam"), TEXT("Rotterdam Nieuwe Maas odağı") },
	{ TEXT("new_orleans"), TEXT("USA / New Orleans"), 29.953, -90.071, 0.03, 0.045, TEXT("new_orleans"), TEXT("New Orleans Mississippi odağı") },
	{ TEXT("grindavik"), TEXT("Ísland / Grindavík–Reykjanes"), 63.844, -22.435, 0.06, 0.1, TEXT("grindavik"), TEXT("Grindavík–Sundhnúkar yerel odağı") },
	{ TEXT("vesuvius"), TEXT("Italia / Napoli–Vezüv"), 40.803, 14.375, 0.045, 0.065, TEXT("torre_del_greco"), TEXT("Torre del Greco–Vezüv yerel odağı") },
	{ TEXT("christchurch"), TEXT("Aotearoa New Zealand / Christchurch"), -43.53, 172.65, 0.03, 0.05, TEXT("christchurch"), TEXT("Christchurch Avon–alüvyon odağı") },
	{ TEXT("kathmandu"), TEXT("Nepal / Kathmandu vadisi"), 27.71, 85.32, 0.035, 0.045, TEXT("kathmandu"), TEXT("Kathmandu vadi ve akarsu odağı") },
	{ TEXT("jakarta"), TEXT("Indonesia / Jakarta deltası"), -6.115, 106.845, 0.03, 0.04, TEXT("jakarta"), TEXT("Jakarta kuzey delta odağı") },
};
static constexpr FRegionFocusRange GeographicRegions[] = {
	{ 0, 1 }, // atakent
	{ 1, 1 }, // dereyolu
	{ 2, 1 }, // yelova
	{ 3, 1 }, // kiyikent
	{ 4, 1 }, // mugla
	{ 5, 1 }, // etna
	{ 6, 1 }, // konya
	{ 7, 4 }, // miami_keys
	{ 11, 3 }, // cyprus
	{ 14, 1 }, // yakutsk
	{ 15, 1 }, // valparaiso
	{ 16, 1 }, // valdivia
	{ 17, 1 }, // sendai
	{ 18, 1 }, // rotterdam
	{ 19, 1 }, // new_orleans
	{ 20, 1 }, // grindavik
	{ 21, 1 }, // vesuvius
	{ 22, 1 }, // christchurch
	{ 23, 1 }, // kathmandu
	{ 24, 1 }, // jakarta
};
static const FAftershockLocalFocusRules GeographicFocusRules[] = {
	{ false, true, false, EAftershockCoastEdge::None, { 500.0f, 0.5f, 0.15f, 0.12f, 0.95f, 0.25f, 20.0f, 0.5f } },
	{ true, true, false, EAftershockCoastEdge::North, { 850.0f, 0.85f, 0.65f, 0.35f, 0.4f, 0.65f, 110.0f, 0.5f } },
	{ false, true, false, EAftershockCoastEdge::None, { 800.0f, 0.35f, 0.5f, 0.25f, 0.9f, 1.0f, 35.0f, 0.5f } },
	{ true, true, false, EAftershockCoastEdge::West, { 450.0f, 0.5f, 0.2f, 0.65f, 0.95f, 0.5f, 50.0f, 0.5f } },
	{ true, true, false, EAftershockCoastEdge::South, { 750.0f, 0.25f, 0.95f, 0.15f, 0.6f, 0.55f, 40.0f, 0.5f } },
	{ false, false, true, EAftershockCoastEdge::None, { 1000.0f, 0.3f, 0.2f, 0.15f, 0.85f, 0.55f, 25.0f, 0.5f } },
	{ false, false, false, EAftershockCoastEdge::None, { 140.0f, 0.15f, 0.06f, 0.95f, 0.35f, 0.35f, 70.0f, 0.5f } },
	{ true, true, false, EAftershockCoastEdge::East, { 65.0f, 0.75f, 0.12f, 0.1f, 0.05f, 0.25f, 75.0f, 0.45f } },
	{ true, false, false, EAftershockCoastEdge::East, { 40.0f, 0.65f, 0.35f, 0.08f, 0.05f, 0.45f, 75.0f, 0.5f } },
	{ true, false, false, EAftershockCoastEdge::South, { 35.0f, 0.6f, 0.18f, 0.05f, 0.05f, 0.55f, 75.0f, 0.5f } },
	{ true, false, false, EAftershockCoastEdge::South, { 35.0f, 0.6f, 0.1f, 0.04f, 0.05f, 0.6f, 75.0f, 0.5f } },
	{ false, false, false, EAftershockCoastEdge::None, { 320.0f, 0.23f, 0.3f, 0.75f, 0.6f, 0.45f, 100.0f, 0.5f } },
	{ false, false, false, EAftershockCoastEdge::None, { 120.0f, 0.22f, 0.1f, 0.85f, 0.55f, 0.3f, 100.0f, 0.5f } },
	{ false, true, false, EAftershockCoastEdge::None, { 600.0f, 0.35f, 0.55f, 0.65f, 0.65f, 0.55f, 110.0f, 0.5f } },
	{ false, true, false, EAftershockCoastEdge::None, { 120.0f, 0.65f, 0.4f, 0.2f, 0.25f, 0.35f, 10.0f, 0.7f } },
	{ true, false, false, EAftershockCoastEdge::West, { 850.0f, 0.3f, 0.65f, 0.15f, 0.95f, 0.7f, 315.0f, 0.5f } },
	{ false, true, false, EAftershockCoastEdge::None, { 170.0f, 0.85f, 0.6f, 0.45f, 0.95f, 0.4f, 285.0f, 0.45f } },
	{ true, true, false, EAftershockCoastEdge::East, { 180.0f, 0.65f, 0.2f, 0.7f, 0.95f, 0.35f, 95.0f, 0.6f } },
	{ false, true, false, EAftershockCoastEdge::None, { 45.0f, 0.75f, 0.08f, 0.2f, 0.1f, 0.35f, 250.0f, 0.55f } },
	{ false, true, false, EAftershockCoastEdge::None, { 45.0f, 0.8f, 0.15f, 0.15f, 0.1f, 0.25f, 120.0f, 0.65f } },
	{ true, false, true, EAftershockCoastEdge::South, { 650.0f, 0.45f, 0.0f, 0.03f, 0.95f, 0.9f, 240.0f, 0.5f } },
	{ true, false, true, EAftershockCoastEdge::West, { 950.0f, 0.45f, 0.4f, 0.55f, 0.75f, 0.45f, 240.0f, 0.5f } },
	{ false, true, false, EAftershockCoastEdge::None, { 100.0f, 0.7f, 0.2f, 0.45f, 0.95f, 0.4f, 280.0f, 0.6f } },
	{ false, true, false, EAftershockCoastEdge::None, { 600.0f, 0.55f, 0.2f, 0.5f, 1.0f, 0.65f, 120.0f, 0.35f } },
	{ true, true, false, EAftershockCoastEdge::North, { 70.0f, 0.9f, 0.1f, 0.15f, 0.65f, 0.25f, 315.0f, 0.4f } },
};
static const FAftershockRegionRules GeographicRegionRules[] = {
	{ TEXT("atakent"), EDisasterType::Earthquake, { 1.0f, 0.35f, 0.25f, 0.1f, 0.0f, 0.0f, 0.25f }, false, true, false, false, { 500.0f, 0.5f, 0.15f, 0.12f, 0.95f, 0.25f, 20.0f, 0.5f }, { 28.715, 40.996, 28.835, 41.086 }, 0.0, 0.0, TEXT("Legacy scenario 0 geometry, anchor and sampled bytes are preserved. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("dereyolu"), EDisasterType::Flood, { 0.3f, 1.0f, 0.45f, 0.2f, 0.0f, 0.0f, 0.15f }, true, true, false, false, { 850.0f, 0.85f, 0.65f, 0.35f, 0.4f, 0.65f, 110.0f, 0.5f }, { 40.052, 40.867, 40.172, 40.957 }, 0.0, 0.0, TEXT("Legacy scenario 1 terrain is preserved. Coastal context in the broad local DEM window is not an enabled tsunami scenario. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("yelova"), EDisasterType::Wind, { 0.9f, 0.35f, 1.0f, 0.5f, 0.0f, 0.0f, 0.5f }, false, true, false, false, { 800.0f, 0.35f, 0.5f, 0.25f, 0.9f, 1.0f, 35.0f, 0.5f }, { 36.13, 36.445, 36.25, 36.535 }, 0.0, 0.0, TEXT("Legacy scenario 2 wind corridor is preserved; relief does not guarantee earthquake safety. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("kiyikent"), EDisasterType::Tsunami, { 0.9f, 0.55f, 0.5f, 0.25f, 1.0f, 0.0f, 0.45f }, true, true, false, false, { 450.0f, 0.5f, 0.2f, 0.65f, 0.95f, 0.5f, 50.0f, 0.5f }, { 35.92, 36.045, 36.04, 36.135 }, 0.0, 0.0, TEXT("Legacy scenario 3 tsunami coastline is preserved; a hypothetical event, not a historical reconstruction. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("mugla"), EDisasterType::Fire, { 0.55f, 0.35f, 0.55f, 1.0f, 0.0f, 0.0f, 0.85f }, true, true, false, false, { 750.0f, 0.25f, 0.95f, 0.15f, 0.6f, 0.55f, 40.0f, 0.5f }, { 28.214, 36.81, 28.334, 36.9 }, 0.0, 0.0, TEXT("Legacy scenario 4 authored forest is preserved. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("etna"), EDisasterType::Volcano, { 0.65f, 0.3f, 0.45f, 0.35f, 0.0f, 1.0f, 0.35f }, false, false, true, false, { 1000.0f, 0.3f, 0.2f, 0.15f, 0.85f, 0.55f, 25.0f, 0.5f }, { 14.935, 37.66, 15.095, 37.8 }, 37.748, 14.999, TEXT("Mapped volcano reference is separate from the city anchor; warming is not an eruption trigger. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("konya"), EDisasterType::Drought, { 0.25f, 0.2f, 0.4f, 0.25f, 0.0f, 0.0f, 1.0f }, false, false, false, false, { 140.0f, 0.15f, 0.06f, 0.95f, 0.35f, 0.35f, 70.0f, 0.5f }, { 33.49, 37.67, 33.61, 37.76 }, 0.0, 0.0, TEXT("Legacy scenario 6 agriculture is preserved; drought is a water/crop stress event, not building collapse. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("miami_keys"), EDisasterType::Wind, { 0.0f, 0.9f, 1.0f, 0.15f, 0.0f, 0.0f, 0.35f }, true, true, false, true, { 65.0f, 0.75f, 0.12f, 0.1f, 0.05f, 0.25f, 75.0f, 0.45f }, { -81.9, 24.4, -80.05, 25.95 }, 0.0, 0.0, TEXT("Four separate local DEM focuses; the regional corridor is hundreds of kilometres, not one 250m grid. Storm surge is a distinct unsupported mechanism, never a renamed tsunami.") },
	{ TEXT("cyprus"), EDisasterType::Drought, { 0.65f, 0.3f, 0.45f, 0.8f, 0.0f, 0.0f, 1.0f }, false, false, false, false, { 320.0f, 0.23f, 0.3f, 0.75f, 0.6f, 0.45f, 100.0f, 0.5f }, { 32.77, 35.06, 33.3, 35.3 }, 0.0, 0.0, TEXT("Three separate local inland focuses, not one merged settlement. The broad regional bounds are for navigation/context only. Local hydrography and ecology are authored overlays.") },
	{ TEXT("yakutsk"), EDisasterType::Flood, { 0.25f, 1.0f, 0.35f, 0.45f, 0.0f, 0.0f, 0.25f }, false, true, false, false, { 120.0f, 0.65f, 0.4f, 0.2f, 0.25f, 0.35f, 10.0f, 0.7f }, { 129.68, 62.0, 129.79, 62.07 }, 0.0, 0.0, TEXT("River flooding context. Ice-jam, freeze/thaw and permafrost engineering are not implemented solvers. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("valparaiso"), EDisasterType::Earthquake, { 1.0f, 0.3f, 0.45f, 0.85f, 0.8f, 0.0f, 0.55f }, true, false, false, false, { 850.0f, 0.3f, 0.65f, 0.15f, 0.95f, 0.7f, 315.0f, 0.5f }, { -71.662, -33.078, -71.572, -33.008 }, 0.0, 0.0, TEXT("Coastal evacuation and hillside fire are separate authored hazards; no historical event replay. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("valdivia"), EDisasterType::Earthquake, { 1.0f, 0.85f, 0.4f, 0.25f, 0.0f, 0.0f, 0.2f }, false, true, false, false, { 170.0f, 0.85f, 0.6f, 0.45f, 0.95f, 0.4f, 285.0f, 0.45f }, { -73.295, -39.852, -73.195, -39.782 }, 0.0, 0.0, TEXT("Inland river focus. Tsunami is not enabled merely because the wider Chilean region has a coast. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("sendai"), EDisasterType::Tsunami, { 0.95f, 0.65f, 0.45f, 0.15f, 1.0f, 0.0f, 0.2f }, true, true, false, false, { 180.0f, 0.65f, 0.2f, 0.7f, 0.95f, 0.35f, 95.0f, 0.6f }, { 140.95, 38.22, 141.03, 38.29 }, 0.0, 0.0, TEXT("Coastal plain focus, not the whole Sendai municipality. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("rotterdam"), EDisasterType::Flood, { 0.0f, 1.0f, 0.85f, 0.05f, 0.0f, 0.0f, 0.25f }, false, true, false, true, { 45.0f, 0.75f, 0.08f, 0.2f, 0.1f, 0.35f, 250.0f, 0.55f }, { 3.8, 51.75, 4.75, 52.1 }, 0.0, 0.0, TEXT("Default focus is an inland estuary/river reach, not an open ocean coastline. Coastal surge, levee and pump hydraulics require separate implementation.") },
	{ TEXT("new_orleans"), EDisasterType::Wind, { 0.0f, 1.0f, 0.95f, 0.1f, 0.0f, 0.0f, 0.25f }, false, true, false, true, { 45.0f, 0.8f, 0.15f, 0.15f, 0.1f, 0.25f, 120.0f, 0.65f }, { -90.3, 29.7, -89.5, 30.2 }, 0.0, 0.0, TEXT("Default neighbourhood has river/drainage context, not an ocean edge. Rain, river and coastal surge are distinct; pumps, levee failure and subsidence are not yet solvers.") },
	{ TEXT("grindavik"), EDisasterType::Volcano, { 0.9f, 0.1f, 0.9f, 0.0f, 0.0f, 1.0f, 0.1f }, true, false, true, false, { 650.0f, 0.45f, 0.0f, 0.03f, 0.95f, 0.9f, 240.0f, 0.5f }, { -22.535, 63.784, -22.335, 63.904 }, 63.8833333333, -22.3833333333, TEXT("Volcano coordinate is the GVP Sundhnukar crater-row subfeature rounded to one arc-minute, not a prediction of a future fissure. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("vesuvius"), EDisasterType::Volcano, { 0.6f, 0.3f, 0.4f, 0.4f, 0.0f, 1.0f, 0.45f }, true, false, true, false, { 950.0f, 0.45f, 0.4f, 0.55f, 0.75f, 0.45f, 240.0f, 0.5f }, { 14.15, 40.72, 14.53, 40.95 }, 40.821, 14.426, TEXT("Local focus is Torre del Greco/volcano surroundings, not all Naples. Lava/ash gameplay does not model pyroclastic density currents or real evacuation forecasts.") },
	{ TEXT("christchurch"), EDisasterType::Earthquake, { 1.0f, 0.75f, 0.45f, 0.25f, 0.0f, 0.0f, 0.35f }, false, true, false, false, { 100.0f, 0.7f, 0.2f, 0.45f, 0.95f, 0.4f, 280.0f, 0.6f }, { 172.6, -43.56, 172.7, -43.5 }, 0.0, 0.0, TEXT("Inland Avon/alluvial focus; earthquake soil effects use authored classes, not a parcel liquefaction survey. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("kathmandu"), EDisasterType::Earthquake, { 1.0f, 0.65f, 0.35f, 0.2f, 0.0f, 0.0f, 0.35f }, false, true, false, false, { 600.0f, 0.55f, 0.2f, 0.5f, 1.0f, 0.65f, 120.0f, 0.35f }, { 85.275, 27.675, 85.365, 27.745 }, 0.0, 0.0, TEXT("Valley access and river flooding are authored overlays; landslide and rockfall are not implemented solvers. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
	{ TEXT("jakarta"), EDisasterType::Flood, { 0.45f, 1.0f, 0.5f, 0.1f, 0.0f, 0.0f, 0.35f }, true, true, false, true, { 70.0f, 0.9f, 0.1f, 0.15f, 0.65f, 0.25f, 315.0f, 0.4f }, { 106.805, -6.145, 106.885, -6.085 }, 0.0, 0.0, TEXT("North Jakarta local delta, not the entire megacity. Subsidence, tidal surge and pipe/levee hydraulics require separate implementations. Sparse real elevation, compressed authored neighbourhood. Rivers, soil, ecology and hazards are authored scenario overlays, not a cadastral/geological survey or forecast.") },
};
