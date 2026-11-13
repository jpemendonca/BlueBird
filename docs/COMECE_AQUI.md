# Comece aqui

O BlueBird toca o som do PS5 numa caixa de som ou fone Bluetooth, usando um adaptador Bluetooth USB plugado no console.

## O que você precisa

- Um PS5 desbloqueado com um carregador de payload.
- Um adaptador Bluetooth USB. O TP-Link UB500 e os adaptadores genéricos "CSR 4.0" servem.
- Uma caixa de som ou fone Bluetooth.

O BlueBird nunca usa o Bluetooth do próprio PS5. Ele fica com o DualSense, que continua funcionando normal.

## Passo a passo

1. Baixe o `BlueBird.elf` da última versão.
2. Mande para o PS5 pelo seu carregador e **inicie**. Aparece a notificação "BlueBird … ativo".
3. Plugue o adaptador Bluetooth USB no PS5. Aparece "adaptador Bluetooth pronto".
4. Abra o **BlueBird** na aba Mídia. Ou abra `http://<IP_DO_PS5>:8120` no celular.
5. Coloque a caixa em modo de pareamento. Normalmente é segurar o botão Bluetooth até a luz piscar rápido.
6. Toque em **Procurar**. Quando a caixa aparecer, toque em **Conectar**. Aparece "tocando em …".

Da próxima vez é só ligar a caixa. O BlueBird chama a última caixa sozinho.

## Sobre o som

- A TV continua tocando também. Abaixe o volume na TV.
- O alto-falante do controle continua funcionando, porque nada fica plugado no controle. Isso vale também para fones: o som que o jogo manda para o controle ainda sai pelo controle. Um modo fone, que leva esse som para o fone, está sendo estudado: em **Diagnóstico**, o botão **Procurar o áudio do controle** faz o teste.
- O Bluetooth tem um atraso de uns 150 a 250 ms. Por isso o som do controle chega antes do som da caixa.

## Usando junto com o OmniPad

Cada payload precisa do seu próprio adaptador. Plugue dois. No painel do BlueBird, em **Adaptador Bluetooth**, escolha qual ele usa.

## Se der problema

- **"Nenhum adaptador encontrado":** tire e plugue o adaptador de novo, de preferência direto no PS5.
- **A caixa não aparece na busca:** confira se ela está piscando em modo de pareamento e procure de novo.
- **"não respondeu":** a caixa pode estar desligada, longe, ou conectada no celular. Desconecte do celular e tente de novo.
- **Conecta mas não sai som:** toque em **Tom de teste**. Se o tom sai, o Bluetooth está certo e o problema é na captura do áudio do console.

Em qualquer caso, abra **Diagnóstico** no painel, toque em **Copiar diagnóstico** e cole numa issue no GitHub.
