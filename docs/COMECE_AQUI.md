# Comece aqui

O BlueBird toca o som do PS5 numa caixa de som ou num fone Bluetooth, usando um adaptador Bluetooth USB plugado no console.

## O que você precisa

- Um PS5 desbloqueado com um carregador de payload.
- Um adaptador Bluetooth USB. O TP-Link UB500 e os adaptadores genéricos "CSR 4.0" servem.
- Um dispositivo de áudio Bluetooth: caixa de som ou fone.

O BlueBird nunca usa o Bluetooth do próprio PS5. Ele fica com o DualSense, que continua funcionando normal.

## Passo a passo

1. Baixe o `BlueBird.elf` da [última versão](https://github.com/jpemendonca/BlueBird/releases/latest).
2. Mande para o PS5 pelo seu carregador e **inicie** (Launch). Aparece a notificação "BlueBird … ativo".
3. Plugue o adaptador Bluetooth USB no PS5. Aparece "adaptador Bluetooth pronto".
4. Abra o **BlueBird** na aba Mídia. Ou abra `http://<IP_DO_PS5>:8120` no celular.
5. Coloque a caixa ou o fone em modo de pareamento. Normalmente é segurar o botão Bluetooth até a luz piscar rápido.
6. Toque em **Procurar**. Quando o dispositivo aparecer, toque em **Conectar**. Aparece "tocando em …".

Da próxima vez é só ligar o dispositivo. O BlueBird chama o último sozinho.

Se você abrir o tile com o BlueBird parado, aparece uma página explicando que é preciso iniciar o BlueBird primeiro. Ela abre o painel sozinha quando o BlueBird responder. Essa página fica guardada no navegador do PS5 depois que você abre o tile uma vez com o BlueBird rodando.

O painel abre no idioma do console. Para trocar, use o seletor de idioma no canto de cima.

## Sobre o som

- **O alto-falante do controle continua tocando.** Alguns jogos mandam sons para o alto-falante do DualSense, e o PS5 continua tocando esses sons nele, mesmo com fone. O BlueBird leva o som principal do console, e o PS5 não mistura o som do controle nele. Se isso incomodar, desative em **Configurações > Acessórios > Controles > Volume do alto-falante do controle**.
- A TV continua tocando também. Abaixe o volume na TV.
- O Bluetooth tem um atraso de uns 150 a 250 ms, conforme a caixa ou o fone.

## Usando junto com o OmniPad

Cada payload precisa do seu próprio adaptador. Plugue dois. No painel do BlueBird, em **Adaptador Bluetooth**, escolha qual ele usa.

## Se der problema

- **"Nenhum adaptador encontrado":** tire e plugue o adaptador de novo, de preferência direto no PS5.
- **O dispositivo não aparece na busca:** confira se ele está piscando em modo de pareamento e procure de novo.
- **"não respondeu":** o dispositivo pode estar desligado, longe, ou conectado no celular. Desconecte do celular e tente de novo.
- **Conecta mas não sai som:** toque em **Tom de teste**. Se o tom sai, o Bluetooth está certo e o problema é na captura do áudio do console.

Em qualquer caso, abra **Ajuda** no painel e toque em **Enviar relatório**. Aparece um código: coloque ele na sua mensagem ou numa issue no GitHub. Sem internet no PS5, abra o painel no celular e envie de lá, ou use **Copiar diagnóstico** e cole numa issue.

O relatório só sai do console quando você aperta Enviar. Endereços MAC são cortados antes de salvar, e o relatório é apagado em 90 dias.
