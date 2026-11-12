## Známe obmedzenia
- Žiadne congestion control a adaptívne RTT (fixné retransmit intervaly).
- Server nezapamätáva out-of-order segmenty (očakáva GBN od klienta).
- Jedno spojenie na proces (žiadny multi-client server).
- Bez šifrovania a autentizácie.

## Funkcionalita
- Spoľahlivý prenos dát nad UDP s vlastným protokolom inšpirovaným TCP
- Go-Back-N (GBN) algoritmus s oknom veľkosti 16 segmentov
- 3-way handshake pre nadviazanie spojenia (SYN, SYN|ACK, ACK)
- Explicitné ukončenie spojenia (FIN/ACK)
- Verifikácia integrity dát pomocou ones' complement checksum
- Retransmisia stratených paketov s intervalом 200 ms
- Inaktívny timeout pre detekciu zlyhania komunikácie
- Klientský režim (-c) s podporou odosielania dát z stdin alebo súboru
- Serverový režim (-s) s príjmom dát a zápisom do stdout alebo súboru
- Kumualtívne potvrdenia (ACK) s bajtovými sekvenčnými číslami
- Ošetrenie SIGINT a SIGTERM signálov pre graciózne ukončenie