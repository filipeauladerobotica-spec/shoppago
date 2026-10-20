# shope pago

Loja online independente shope pago, com frontend em React/JSX e CSS e API em C. O catálogo começa vazio: somente produtos publicados por uma conta aparecem na loja.

## Executar com Docker

Na pasta `config/shoppago`:

```bash
docker build -t shope-pago .
docker volume create shope_pago_data
docker run --rm -p 8081:8080 -v shope_pago_data:/data shope-pago
```

Abra `http://localhost:8081`. Contas, sessões, anúncios, sacolas e pedidos ficam no SQLite dentro do volume `shope_pago_data`. Faça backup do volume antes de atualizar ou mover o serviço. Se estiver atualizando uma instalação que já usa o volume `vitrine_data`, continue usando esse mesmo nome para preservar os dados existentes.

O frontend é desenvolvido em React (`App.jsx`) e CSS (`app.css`); durante a imagem Docker o Vite compila esses arquivos para os recursos estáticos servidos pela API em C.

Para compilar somente o frontend localmente, com Node.js 22 ou superior:

```bash
npm ci
npm run build
```

Para desenvolver a interface com recarga automática, deixe a API acessível em `http://127.0.0.1:8081` e execute `npm run dev`. Se a API estiver em outro endereço, defina `SHOPE_PAGO_API_URL` antes de iniciar o Vite.

Para publicar atrás de um proxy HTTPS, defina `COOKIE_SECURE=1` no contêiner e encaminhe HTTPS ao servidor. Configure também `PUBLIC_BASE_URL` com a URL HTTPS pública, sem barra no final. Não exponha o serviço à internet sem HTTPS.

## Login com Google (opcional)

O acesso Google é opcional. A janela de conta mostra o botão “Continuar com Google” e informa quando ele ainda não está ativo; o botão só inicia o fluxo real depois que as três variáveis abaixo estiverem configuradas. Sem elas, o cadastro e o login por e-mail continuam disponíveis.

1. No Google Cloud Console, crie um cliente OAuth do tipo **Aplicativo da Web** e configure a tela de consentimento.
2. Cadastre como URI de redirecionamento autorizada a URL pública exata terminada em `/auth/google/callback` (por exemplo, `https://loja.exemplo.com/auth/google/callback`). Essa URL deve ser HTTPS em produção.
3. Passe `GOOGLE_CLIENT_ID`, `GOOGLE_CLIENT_SECRET` e `GOOGLE_REDIRECT_URI` ao contêiner, sem colocar o segredo no HTML ou no repositório. Exemplo com variáveis já definidas no ambiente:

```bash
docker run --rm -p 8081:8080 \
  -e GOOGLE_CLIENT_ID \
  -e GOOGLE_CLIENT_SECRET \
  -e GOOGLE_REDIRECT_URI \
  -e COOKIE_SECURE=1 \
  -v shope_pago_data:/data shope-pago
```

O backend troca o código OAuth por HTTPS, valida estado de uso único contra CSRF e só aceita perfil Google com e-mail verificado. Uma conta local com o mesmo e-mail verificado é vinculada à identidade Google; um endereço já associado a outra conta Google é recusado. Use sempre um `GOOGLE_REDIRECT_URI` que coincida exatamente com o cadastrado no Console.

### Configuração local

O arquivo `.env.example` já contém o Client ID da loja e o redirecionamento local. Copie-o para `.env`, informe o `GOOGLE_CLIENT_SECRET` apenas nesse arquivo local e inicie o contêiner com:

```bash
docker run --rm --env-file .env -p 8081:8080 \
  -v shope_pago_data:/data shope-pago
```

No Google Cloud, cadastre `http://127.0.0.1:8081/auth/google/callback` como URI de redirecionamento autorizada. O `.env` é ignorado pelo Git e não deve ser enviado ao repositório.

Como alternativa, com Docker Compose instalado, basta executar:

```bash
docker compose up --build
```

O `compose.yaml` carrega o `.env` automaticamente. Quando a API iniciar com as três variáveis OAuth preenchidas, o botão **Entrar com Google** será habilitado.

## Checkout, pagamentos e entrega

O checkout usa o **Mercado Pago Checkout Pro**. O comprador informa o endereço de entrega e é redirecionado ao Mercado Pago para pagar; o pedido só muda para pago após o backend consultar e confirmar a notificação assinada do Mercado Pago. O retorno do navegador, sozinho, nunca confirma pagamento.

1. Crie uma aplicação no painel do Mercado Pago e comece com credenciais de teste (`MP_ACCESS_TOKEN`).
2. Configure os Webhooks para o evento **Pagamentos**, apontando para `https://seu-dominio/api/payments/mercadopago/webhook`. Copie a chave secreta gerada para `MP_WEBHOOK_SECRET`.
3. Defina `PUBLIC_BASE_URL` como a origem HTTPS pública, sem barra final, e `COOKIE_SECURE=1`.
4. Preencha as variáveis no `.env` e reinicie/recrie o contêiner. O checkout permanece desabilitado enquanto o token, a assinatura do webhook e a URL HTTPS não estiverem configurados.
5. Valide aprovação, recusa e notificações no modo de teste antes de usar credenciais de produção.

O `.env.example` contém apenas campos vazios para segredos. Não coloque tokens em arquivos versionados ou no frontend; mantenha-os no ambiente do backend.

Cada produto publicado tem um campo de **frete por unidade** definido pelo vendedor. O frete é multiplicado pela quantidade, somado ao total e enviado ao Mercado Pago. O comprador informa nome, telefone e endereço; o vendedor só vê os dados completos de entrega depois da aprovação do pagamento. Após receber o pedido, o vendedor pode registrar envio, código de rastreio opcional e confirmação de entrega. O comprador acompanha essas etapas em **Meus pedidos**.

## Funções implementadas

- Criar conta e entrar com e-mail e senha. As senhas são derivadas com PBKDF2-SHA256; as sessões usam tokens aleatórios guardados em cookie `HttpOnly` e `SameSite=Lax`.
- Login Google OAuth opcional, ativado apenas quando configurado pelo operador.
- Checkout Pro do Mercado Pago com confirmação server-to-server por Webhook assinado; credenciais e domínio HTTPS são exigidos para habilitar pagamentos.
- Endereço de entrega coletado no checkout; frete por unidade definido pelo vendedor e incluído no total antes do pagamento.
- Pedidos com status de pagamento e entrega, código de rastreio opcional e painel do vendedor para atualizar envio e entrega.
- É necessário entrar na conta para adicionar produtos à sacola, comprar ou publicar anúncios.
- Para vender, a conta completa um cadastro em etapas com CPF validado, nascimento, telefone e categoria de interesse. O CPF é validado pela API e descartado; telefone, nascimento e categoria ficam no perfil do vendedor.
- O painel mostra anúncios ativos, pedidos recebidos e receita confirmada, com gráfico mensal. A receita só é contabilizada após aprovação confirmada pelo Mercado Pago.
- O painel do vendedor mostra a imagem, o nome e a média/quantidade de estrelas de cada anúncio. Compradores com pagamento aprovado podem avaliar de 1 a 5 estrelas; cada comprador tem uma avaliação por produto, que pode atualizar depois.
- Selecionar a imagem ou o nome de um produto abre uma página de detalhes com imagem, categoria, descrição, média de avaliações, preço e opção para adicionar ao carrinho.
- Produtos são publicados com nome, descrição, categoria, preço e link HTTPS obrigatório para a imagem; o telefone salvo no perfil aparece automaticamente no formulário.
- Catálogo com filtro por categoria e ordenação por data, preço ou avaliação; opção de tentar novamente se o carregamento falhar. Imagens indisponíveis mostram um marcador alternativo.
- Vendedor pode ver e remover apenas os próprios anúncios.
- Sacola e pedidos são associados à conta e permanecem no banco entre reinicializações do contêiner.

## Limitações — sem promessas fictícias

- O frete é um valor simples por unidade informado pelo vendedor; não há cotação automática dos Correios, etiqueta, seguro ou coleta.
- As notificações de pagamento exigem endpoint HTTPS publicamente acessível e configuração correta do segredo no painel Mercado Pago. Teste o webhook antes de aceitar pagamentos reais.
- Reembolsos, cancelamentos automatizados, disputas, comissões e repasses aos vendedores ainda não estão implementados.
- Avaliações ficam disponíveis após a confirmação do pagamento; não é necessário aguardar a entrega.
- Imagens são exibidas a partir de um link HTTPS fornecido pelo vendedor; não há envio de arquivos.
- Ainda não existem verificação de e-mail, recuperação de senha, proteção contra tentativas repetidas de login, painel administrativo ou fluxo de devolução.

Antes de aceitar vendas, complete os controles de produção, incluindo limitação de tentativas, recuperação de conta, política de privacidade, política de reembolso, revisão legal e backups restauráveis. Não use dados reais de clientes enquanto esses controles não estiverem prontos.
