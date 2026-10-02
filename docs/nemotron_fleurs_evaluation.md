# Resultado de evaluación Nemotron en FLEURS

## Corrida completada

El artefacto `fleurs-es_419-test__nemotron-560-600-2__v1-20261002T194301Z-1-001.zip`
contiene una corrida completa del 2 de octubre de 2026:

- Dataset: `google/fleurs`, configuración `es_419`, split `test`, revisión
  `70bb2e84b976b7e960aa89f1c648e09c59f894dd`.
- 908 grabaciones únicas. Los IDs de frase se repiten para lectores distintos;
  el evaluador identifica cada grabación por frase y nombre de archivo.
- Offline: 908/908; streaming acelerado: 908/908.
- Configuración: español `es-ES`, Nemotron 3.5 ASR Streaming 0.6B, NeMo
  `2639d4bef8d1450782263a8f616242acfb6fecb9`, 560 ms, EOU 600 ms y 2 tokens
  residuales.
- Runtime registrado: Tesla T4, PyTorch 2.11.0+cu130, NeMo 3.1.0.
- Commit del proyecto: `c53698a3b506a3663ea3d4a557c4fef57dcfc160`.

## WER agregado

| Fase | Perfil histórico `legacy` | Perfil `numeric_es` | Ediciones / palabras de referencia (`numeric_es`) |
|---|---:|---:|---:|
| Offline | 6,44 % | 4,73 % | 1.102 / 23.321 |
| Streaming | 9,11 % | 7,42 % | 1.730 / 23.321 |

Streaming queda aproximadamente 2,69 puntos porcentuales por encima de offline
con `numeric_es`. El 4,26 % publicado por NVIDIA se refiere a FLEURS español en
streaming de 560 ms, pero el normalizador exacto no está publicado; el 7,42 % es
una referencia comparable en corpus y modo general, no una reproducción exacta
del benchmark.

## Integridad y límites

No hubo fallos de clip. En las 908 sesiones streaming se registraron cero errores,
cero eventos descartados y cola de eventos drenada; se guardaron 17.789 eventos
de transcripción. El tiempo de inferencia agregado fue de unos 228 s offline y
1.356 s streaming sobre 3,09 horas de audio. La ejecución streaming fue
acelerada, sin pausas de tiempo real: estos tiempos/RTF no representan latencia
placa-a-HDMI.

La revisión exacta de los pesos del modelo quedó como `null` en
`model_provenance.json`; se fijaron el ID del modelo y el commit de NeMo, pero
este artefacto no permite identificar el snapshot exacto de pesos. Algunos de
los errores mayores incluyen notación técnica como `802.11n`, que el perfil
`numeric_es` actual no canonicaliza completamente.

MediaSpeech se volverá a evaluar por separado con los mismos dos perfiles WER.
La corrida anterior de MediaSpeech (12,19 % WER histórico) permanece como
referencia histórica; la nueva salida v2 tendrá identidad y carpeta propias.
