import React, { useCallback, useEffect, useRef, useState } from "react";
import loginShoppingBag from "./src/assets/login-shopping-bag.png";
import shopLogo from "./src/assets/shope-pago-logo.png";

const EMPTY_CART = { items: [], totalCents: 0 };
const GOOGLE_ACTION_STORAGE_KEY = "shope_pago_google_action";
const ORDER_STATUS_LABELS = {
  awaiting_payment: "Aguardando pagamento",
  paid: "Pagamento aprovado",
  awaiting_shipment: "Aguardando envio",
  shipped: "Enviado",
  delivered: "Entregue",
  rejected: "Pagamento não aprovado",
  pending: "Aguardando confirmação",
  in_process: "Pagamento em análise",
  approved: "Aprovado",
  refunded: "Reembolsado",
  charged_back: "Pagamento contestado",
  cancelled: "Cancelado"
};

function money(cents) {
  return new Intl.NumberFormat("pt-BR", { style: "currency", currency: "BRL" }).format(cents / 100);
}

function orderStatusLabel(status) {
  return ORDER_STATUS_LABELS[status] || status;
}

function ProductImage({ src, alt, className = "", fallbackClassName = "" }) {
  const [failed, setFailed] = useState(false);

  useEffect(() => setFailed(false), [src]);

  if (!src || failed) {
    return <span className={fallbackClassName} aria-hidden="true">▧</span>;
  }

  return <img className={className} src={src} alt={alt} loading="lazy"
    onError={() => setFailed(true)} />;
}

function isValidCpf(value) {
  const cpf = value.replace(/\D/g, "");
  if (cpf.length !== 11 || /^(\d)\1+$/.test(cpf)) return false;

  const calculateDigit = (digits, factor) => {
    const sum = digits.split("").reduce((total, digit, index) => total + Number(digit) * (factor - index), 0);
    const remainder = (sum * 10) % 11;
    return remainder === 10 ? 0 : remainder;
  };

  return calculateDigit(cpf.slice(0, 9), 10) === Number(cpf[9]) &&
    calculateDigit(cpf.slice(0, 10), 11) === Number(cpf[10]);
}

function formatCpf(value) {
  const digits = value.replace(/\D/g, "").slice(0, 11);
  return digits
    .replace(/^(\d{3})(\d)/, "$1.$2")
    .replace(/^(\d{3})\.(\d{3})(\d)/, "$1.$2.$3")
    .replace(/^(\d{3})\.(\d{3})\.(\d{3})(\d{1,2})$/, "$1.$2.$3-$4");
}

function formatPhone(value) {
  const digits = value.replace(/\D/g, "").slice(0, 11);
  if (digits.length === 0) return "";
  if (digits.length <= 2) return `(${digits}`;
  if (digits.length <= 6) return `(${digits.slice(0, 2)}) ${digits.slice(2)}`;
  const subscriber = digits.slice(2);
  const splitAt = digits.length > 10 ? 7 : 6;
  return `(${digits.slice(0, 2)}) ${subscriber.slice(0, splitAt - 2)}-${subscriber.slice(splitAt - 2)}`;
}

function isValidBirthDate(value) {
  const date = new Date(`${value}T00:00:00`);
  return /^\d{4}-\d{2}-\d{2}$/.test(value) &&
    !Number.isNaN(date.getTime()) &&
    date.toISOString().slice(0, 10) === value &&
    date <= new Date();
}

function formatMonth(value) {
  const date = new Date(`${value}-02T00:00:00`);
  return new Intl.DateTimeFormat("pt-BR", { month: "short" }).format(date).replace(".", "");
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    credentials: "same-origin",
    ...options,
    headers: {
      ...(options.body ? { "Content-Type": "application/json" } : {}),
      ...options.headers
    }
  });
  let result;
  try {
    result = await response.json();
  } catch {
    throw new Error("O servidor retornou uma resposta inválida.");
  }
  if (!response.ok) {
    const error = new Error(result.error || "A solicitação não foi concluída.");
    error.status = response.status;
    throw error;
  }
  return result;
}

function ProductRating({ product, onRate }) {
  const average = Number(product.ratingAverage) || 0;
  const count = Number(product.ratingCount) || 0;
  const roundedAverage = Math.round(average);

  return (
    <div className="product-rating" aria-label={count
      ? `Avaliação média ${average.toFixed(1)} de 5, ${count} ${count === 1 ? "avaliação" : "avaliações"}`
      : "Produto ainda sem avaliações"}>
      <span className="rating-stars">
        {[1, 2, 3, 4, 5].map(star => onRate
          ? <button key={star} className={star <= roundedAverage ? "is-filled" : ""}
              onClick={() => onRate(product.id, star)} type="button"
              aria-label={`Avaliar ${product.name} com ${star} ${star === 1 ? "estrela" : "estrelas"}`}>★</button>
          : <span key={star} className={star <= roundedAverage ? "is-filled" : ""} aria-hidden="true">★</span>)}
      </span>
      <small>{count ? `${average.toFixed(1)} · ${count} ${count === 1 ? "avaliação" : "avaliações"}` : "Sem avaliações"}</small>
    </div>
  );
}

function ProductCard({ product, onAdd, onOpen, onRate }) {
  return (
    <article className="product-card">
      <button className="product-image product-open-button" onClick={() => onOpen(product)} type="button"
        aria-label={`Ver detalhes de ${product.name}`}>
        {product.imageUrl
          ? <ProductImage src={product.imageUrl} alt={product.name}
              className="product-card-image" fallbackClassName="product-placeholder" />
          : <span className="product-placeholder" aria-hidden="true">▧</span>}
      </button>
      <div className="product-info">
        <span className="product-category">{product.category}</span>
        <button className="product-name product-name-button" onClick={() => onOpen(product)} type="button">{product.name}</button>
        <ProductRating product={product} onRate={onRate} />
        <p className="product-description">{product.description || "Sem descrição."}</p>
        <small className="seller-name">Vendido por {product.sellerName}</small>
        <strong className="product-price">{money(product.priceCents)}</strong>
        <small className="seller-name">Frete: {product.shippingCents ? money(product.shippingCents) : "grátis"} por unidade</small>
        <button className="add-to-cart" onClick={() => onAdd(product.id)} type="button">
          Adicionar ao carrinho
        </button>
      </div>
    </article>
  );
}

function StoreMark({ className = "" }) {
  return <img className={`shop-logo ${className}`} src={shopLogo} alt="" aria-hidden="true" />;
}

function CartIcon() {
  return <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8"
    strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
    <circle cx="9" cy="20" r="1" /><circle cx="19" cy="20" r="1" />
    <path d="M2 3h2l2.7 12.4a2 2 0 0 0 2 1.6h9.7a2 2 0 0 0 2-1.6L22 7H5" />
  </svg>;
}

function App() {
  const [products, setProducts] = useState([]);
  const [productsLoading, setProductsLoading] = useState(true);
  const [productsError, setProductsError] = useState("");
  const [search, setSearch] = useState("");
  const [searchOpen, setSearchOpen] = useState(false);
  const [searchOpening, setSearchOpening] = useState(false);
  const [searchClosing, setSearchClosing] = useState(false);
  const [searchAnimationStyle, setSearchAnimationStyle] = useState({});
  const [searchResults, setSearchResults] = useState([]);
  const [searchLoading, setSearchLoading] = useState(false);
  const [searchError, setSearchError] = useState("");
  const [selectedCategory, setSelectedCategory] = useState("");
  const [sortBy, setSortBy] = useState("newest");
  const [user, setUser] = useState(null);
  const [sellerProfile, setSellerProfile] = useState(null);
  const [cart, setCart] = useState(EMPTY_CART);
  const [googleEnabled, setGoogleEnabled] = useState(false);
  const [paymentEnabled, setPaymentEnabled] = useState(false);
  const [authMode, setAuthMode] = useState("login");
  const [authMessage, setAuthMessage] = useState("");
  const [productMessage, setProductMessage] = useState("");
  const [pendingAction, setPendingAction] = useState(null);
  const [menuOpen, setMenuOpen] = useState(false);
  const [navigationOpen, setNavigationOpen] = useState(false);
  const [cartOpen, setCartOpen] = useState(false);
  const [selectedProduct, setSelectedProduct] = useState(null);
  const [toast, setToast] = useState("");
  const [myProducts, setMyProducts] = useState([]);
  const [sellerDashboard, setSellerDashboard] = useState(null);
  const [onboardingStep, setOnboardingStep] = useState(0);
  const [onboardingDraft, setOnboardingDraft] = useState({ cpf: "", birthDate: "", phone: "", interests: "" });
  const [onboardingMessage, setOnboardingMessage] = useState("");
  const [orders, setOrders] = useState([]);
  const [sellerOrders, setSellerOrders] = useState([]);
  const [sellerTrackingCodes, setSellerTrackingCodes] = useState({});
  const [manageMessage, setManageMessage] = useState("");
  const [manageLoading, setManageLoading] = useState(false);
  const [submittingAuth, setSubmittingAuth] = useState(false);
  const [submittingProduct, setSubmittingProduct] = useState(false);
  const [submittingSellerProfile, setSubmittingSellerProfile] = useState(false);
  const [submittingCheckout, setSubmittingCheckout] = useState(false);
  const accountDialog = useRef(null);
  const onboardingDialog = useRef(null);
  const sellerDialog = useRef(null);
  const productsDialog = useRef(null);
  const ordersDialog = useRef(null);
  const checkoutDialog = useRef(null);
  const productDialog = useRef(null);
  const productForm = useRef(null);
  const menuButton = useRef(null);
  const navigationCloseButton = useRef(null);
  const toastTimer = useRef(null);
  const searchRequestId = useRef(0);
  const searchCloseTimer = useRef(null);

  const showToast = useCallback((message) => {
    setToast(message);
    window.clearTimeout(toastTimer.current);
    toastTimer.current = window.setTimeout(() => setToast(""), 3000);
  }, []);

  const loadProducts = useCallback(async (query = "") => {
    setProductsLoading(true);
    setProductsError("");
    try {
      const data = await api(`/api/products?q=${encodeURIComponent(query)}`);
      if (!Array.isArray(data.products)) throw new Error("A resposta do catálogo é inválida.");
      setProducts(data.products);
    } catch (error) {
      setProducts([]);
      setProductsError(error.message);
    } finally {
      setProductsLoading(false);
    }
  }, []);

  useEffect(() => {
    if (!searchOpen) return undefined;
    const requestId = ++searchRequestId.current;
    const query = search.trim();
    setSearchError("");

    if (!query) {
      setSearchResults([]);
      setSearchLoading(false);
      return undefined;
    }

    setSearchLoading(true);
    const fetchResults = async () => {
      try {
        const data = await api(`/api/products?q=${encodeURIComponent(query)}`);
        if (!Array.isArray(data.products)) throw new Error("A resposta da busca é inválida.");
        if (requestId === searchRequestId.current) setSearchResults(data.products);
      } catch (error) {
        if (requestId === searchRequestId.current) {
          setSearchResults([]);
          setSearchError(error.message);
        }
      } finally {
        if (requestId === searchRequestId.current) setSearchLoading(false);
      }
    };
    fetchResults();

    return undefined;
  }, [search, searchOpen]);

  const loadCart = useCallback(async () => {
    try {
      setCart(await api("/api/cart"));
    } catch (error) {
      if (error.status !== 401) showToast(error.message);
    }
  }, [showToast]);

  const loadUser = useCallback(async () => {
    try {
      const data = await api("/api/me");
      setUser(data.user);
      setSellerProfile(data.user?.sellerProfile || null);
      if (data.user) await loadCart();
      else {
        setCart(EMPTY_CART);
        setSellerProfile(null);
      }
      return data.user;
    } catch (error) {
      setUser(null);
      setCart(EMPTY_CART);
      setSellerProfile(null);
      console.error("Não foi possível verificar a sessão da conta.", error);
      return null;
    }
  }, [loadCart]);

  const openAccount = (mode = "login", action = null) => {
    setPendingAction(action);
    setAuthMode(mode);
    setAuthMessage("");
    accountDialog.current?.showModal();
  };

  const openSellerForm = () => {
    if (!user) {
      openAccount("login", { type: "sell" });
      return;
    }
    if (!sellerProfile) {
      setOnboardingStep(0);
      setOnboardingDraft({ cpf: "", birthDate: "", phone: "", interests: "" });
      setOnboardingMessage("");
      onboardingDialog.current?.showModal();
      return;
    }
    openSellerDashboard();
  };

  const openProductForm = () => {
    setProductMessage("");
    productForm.current?.reset();
    sellerDialog.current?.showModal();
  };

  const openProductDetails = product => {
    closeSearch();
    setSelectedProduct(product);
    window.requestAnimationFrame(() => productDialog.current?.showModal());
  };

  const addToCart = useCallback(async (productId) => {
    if (!user) {
      setPendingAction({ type: "add-cart", productId });
      setAuthMode("login");
      setAuthMessage("");
      accountDialog.current?.showModal();
      return;
    }
    try {
      setCart(await api("/api/cart", {
        method: "POST",
        body: JSON.stringify({ productId, quantity: 1 })
      }));
      showToast("Produto adicionado ao seu carrinho.");
    } catch (error) {
      if (error.status === 401) {
        setUser(null);
        setPendingAction({ type: "add-cart", productId });
        setAuthMode("login");
        accountDialog.current?.showModal();
      } else {
        showToast(error.message);
      }
    }
  }, [showToast, user]);

  useEffect(() => {
    loadProducts();
    api("/api/auth/google")
      .then(status => setGoogleEnabled(status.enabled === true))
      .catch(error => console.error("Não foi possível verificar a disponibilidade do login com Google.", error));
    api("/api/payments/config")
      .then(status => setPaymentEnabled(status.enabled === true))
      .catch(error => console.error("Não foi possível verificar a disponibilidade dos pagamentos.", error));

    const query = new URLSearchParams(window.location.search);
    const authResult = query.get("auth");
    const paymentResult = query.get("payment");
    if (authResult || paymentResult)
      window.history.replaceState(null, "", window.location.pathname + window.location.hash);
    if (paymentResult === "success")
      showToast("Retorno do Mercado Pago recebido. A confirmação pode levar alguns instantes.");
    else if (paymentResult === "pending")
      showToast("Pagamento em análise. Atualize seus pedidos para acompanhar.");
    else if (paymentResult === "failure")
      showToast("O pagamento não foi concluído. Você pode tentar novamente em Meus pedidos.");
    loadUser().then(async currentUser => {
      if (authResult === "google-success") {
        showToast("Acesso com Google realizado.");
        let action = null;
        try {
          action = JSON.parse(sessionStorage.getItem(GOOGLE_ACTION_STORAGE_KEY) || "null");
          sessionStorage.removeItem(GOOGLE_ACTION_STORAGE_KEY);
        } catch (error) {
          console.warn("A ação salva para continuar após o login Google é inválida.", error);
        }
        if (action?.type === "sell" && currentUser) {
          if (currentUser.sellerProfile) {
            setSellerProfile(currentUser.sellerProfile);
            openSellerDashboard();
          } else {
            setOnboardingStep(0);
            setOnboardingDraft({ cpf: "", birthDate: "", phone: "", interests: "" });
            setOnboardingMessage("");
            onboardingDialog.current?.showModal();
          }
        }
        if (action?.type === "add-cart" && currentUser &&
            Number.isSafeInteger(action.productId) && action.productId > 0) {
          try {
            setCart(await api("/api/cart", {
              method: "POST",
              body: JSON.stringify({ productId: action.productId, quantity: 1 })
            }));
          } catch (error) {
            showToast(error.message);
          }
        }
      } else if (authResult === "google-conflict") {
        showToast("Este e-mail já está vinculado a outra conta Google.");
      } else if (authResult === "google-error") {
        showToast("Não foi possível entrar com Google. Tente novamente.");
      } else if (authResult === "google-unavailable") {
        showToast("O login com Google ainda não foi configurado nesta loja.");
      }
    });
    return () => window.clearTimeout(toastTimer.current);
  }, [loadProducts, loadUser, showToast]);

  const afterAuthentication = async () => {
    accountDialog.current?.close();
    const currentUser = await loadUser();
    const action = pendingAction;
    setPendingAction(null);
    if (action?.type === "sell" && currentUser) {
      if (currentUser.sellerProfile) {
        setSellerProfile(currentUser.sellerProfile);
        openSellerDashboard();
      } else {
        setOnboardingStep(0);
        setOnboardingDraft({ cpf: "", birthDate: "", phone: "", interests: "" });
        setOnboardingMessage("");
        onboardingDialog.current?.showModal();
      }
    }
    if (action?.type === "add-cart" && currentUser) {
      try {
        setCart(await api("/api/cart", {
          method: "POST",
          body: JSON.stringify({ productId: action.productId, quantity: 1 })
        }));
        showToast("Produto adicionado ao seu carrinho.");
      } catch (error) {
        showToast(error.message);
      }
    }
    showToast("Acesso realizado.");
  };

  const submitAuth = async (event, endpoint) => {
    event.preventDefault();
    const form = event.currentTarget;
    setSubmittingAuth(true);
    setAuthMessage("");
    try {
      const values = Object.fromEntries(new FormData(form));
      await api(endpoint, { method: "POST", body: JSON.stringify(values) });
      form.reset();
      await afterAuthentication();
    } catch (error) {
      setAuthMessage(error.message);
    } finally {
      setSubmittingAuth(false);
    }
  };

  const startGoogleLogin = () => {
    try {
      if (pendingAction) sessionStorage.setItem(GOOGLE_ACTION_STORAGE_KEY, JSON.stringify(pendingAction));
      else sessionStorage.removeItem(GOOGLE_ACTION_STORAGE_KEY);
    } catch (error) {
      console.warn("Não foi possível guardar a ação pendente antes do login Google.", error);
    }
    window.location.assign("/auth/google");
  };

  const submitProduct = async (event) => {
    event.preventDefault();
    const values = Object.fromEntries(new FormData(event.currentTarget));
    const priceCents = Math.round(Number(values.price) * 100);
    const shippingCents = Math.round(Number(values.shipping) * 100);
    if (!Number.isSafeInteger(priceCents) || priceCents < 1 ||
        !Number.isSafeInteger(shippingCents) || shippingCents < 0) {
      setProductMessage("Informe um preço e um frete válidos.");
      return;
    }
    setSubmittingProduct(true);
    setProductMessage("");
    try {
      await api("/api/products", {
        method: "POST",
        body: JSON.stringify({
          name: values.name.trim(),
          description: values.description.trim(),
          category: values.category.trim(),
          priceCents,
          shippingCents,
          imageUrl: values.imageUrl.trim()
        })
      });
      sellerDialog.current?.close();
      await loadProducts(search);
      await openSellerDashboard();
      showToast("Produto publicado no catálogo.");
    } catch (error) {
      setProductMessage(error.message);
    } finally {
      setSubmittingProduct(false);
    }
  };

  const submitSellerStep = async (event) => {
    event.preventDefault();
    setOnboardingMessage("");

    if (onboardingStep === 0) {
      if (!isValidCpf(onboardingDraft.cpf)) {
        setOnboardingMessage("Informe um CPF válido.");
        return;
      }
      setOnboardingStep(1);
      return;
    }
    if (onboardingStep === 1) {
      if (!isValidBirthDate(onboardingDraft.birthDate)) {
        setOnboardingMessage("Informe uma data de nascimento válida.");
        return;
      }
      setOnboardingStep(2);
      return;
    }
    if (onboardingStep === 2) {
      if (![10, 11].includes(onboardingDraft.phone.replace(/\D/g, "").length)) {
        setOnboardingMessage("Informe um telefone com DDD.");
        return;
      }
      setOnboardingStep(3);
      return;
    }
    if (onboardingDraft.interests.trim().length < 2) {
      setOnboardingMessage("Conte qual tipo de produto você mais vende.");
      return;
    }

    setSubmittingSellerProfile(true);
    try {
      const result = await api("/api/seller-profile", {
        method: "POST",
        body: JSON.stringify({
          cpf: onboardingDraft.cpf,
          birthDate: onboardingDraft.birthDate,
          phone: onboardingDraft.phone,
          interests: onboardingDraft.interests.trim()
        })
      });
      setSellerProfile(result.profile);
      onboardingDialog.current?.close();
      showToast("Cadastro de vendedor concluído.");
      await openSellerDashboard();
    } catch (error) {
      setOnboardingMessage(error.message);
    } finally {
      setSubmittingSellerProfile(false);
    }
  };

  const openSellerDashboard = async () => {
    setMenuOpen(false);
    setManageMessage("");
    setManageLoading(true);
    setSellerDashboard(null);
    if (!productsDialog.current?.open) productsDialog.current?.showModal();
    try {
      const [dashboard, productsData, sellerOrdersData] = await Promise.all([
        api("/api/seller-dashboard"),
        api("/api/my-products"),
        api("/api/seller-orders")
      ]);
      setSellerDashboard(dashboard);
      setMyProducts(productsData.products);
      setSellerOrders(sellerOrdersData.orders);
    } catch (error) {
      setManageMessage(error.message);
    } finally {
      setManageLoading(false);
    }
  };

  const removeProduct = async (productId) => {
    try {
      await api(`/api/products/${productId}`, { method: "DELETE" });
      await openSellerDashboard();
      await loadProducts(search);
      showToast("Anúncio removido do catálogo.");
    } catch (error) {
      showToast(error.message);
    }
  };

  const rateProduct = async (productId, rating) => {
    if (!user) {
      openAccount();
      return;
    }
    try {
      const result = await api(`/api/products/${productId}/reviews`, {
        method: "POST",
        body: JSON.stringify({ rating })
      });
      const updateRating = product => product.id === productId
        ? { ...product, ratingAverage: result.ratingAverage, ratingCount: result.ratingCount }
        : product;
      setProducts(current => current.map(updateRating));
      setSearchResults(current => current.map(updateRating));
      setMyProducts(current => current.map(updateRating));
      setSelectedProduct(current => current ? updateRating(current) : null);
      showToast("Sua avaliação foi registrada.");
    } catch (error) {
      showToast(error.message);
    }
  };

  const openOrders = async () => {
    setMenuOpen(false);
    setManageMessage("");
    setManageLoading(true);
    ordersDialog.current?.showModal();
    try {
      const data = await api("/api/orders");
      setOrders(data.orders);
    } catch (error) {
      setManageMessage(error.message);
    } finally {
      setManageLoading(false);
    }
  };

  const openCheckout = () => {
    if (!user) {
      openAccount();
      return;
    }
    checkoutDialog.current?.showModal();
  };

  const updateSellerOrder = async (orderId, status) => {
    try {
      await api(`/api/seller-orders/${orderId}`, {
        method: "POST",
        body: JSON.stringify({
          status,
          trackingCode: sellerTrackingCodes[orderId] || ""
        })
      });
      await openSellerDashboard();
      showToast(status === "shipped" ? "Pedido marcado como enviado." : "Entrega marcada como concluída.");
    } catch (error) {
      showToast(error.message);
    }
  };

  const changeCart = async (productId, remove = false) => {
    try {
      const updated = remove
        ? await api(`/api/cart/${productId}`, { method: "DELETE" })
        : await api("/api/cart", {
          method: "POST",
          body: JSON.stringify({ productId, quantity: 1 })
        });
      setCart(updated);
    } catch (error) {
      showToast(error.message);
    }
  };

  const logout = async () => {
    try {
      await api("/api/logout", { method: "POST" });
      setUser(null);
      setSellerProfile(null);
      setCart(EMPTY_CART);
      setMenuOpen(false);
      showToast("Você saiu da sua conta.");
    } catch (error) {
      showToast(error.message);
    }
  };

  const registerOrder = async (event) => {
    event.preventDefault();
    setSubmittingCheckout(true);
    try {
      const values = Object.fromEntries(new FormData(event.currentTarget));
      const order = await api("/api/orders", {
        method: "POST",
        body: JSON.stringify({
          recipientName: values.recipientName,
          recipientPhone: values.recipientPhone,
          postalCode: values.postalCode,
          state: values.state,
          city: values.city,
          street: values.street,
          addressNumber: values.addressNumber,
          complement: values.complement
        })
      });
      setCart(EMPTY_CART);
      setCartOpen(false);
      checkoutDialog.current?.close();
      window.location.assign(order.checkoutUrl);
    } catch (error) {
      showToast(error.message);
    } finally {
      setSubmittingCheckout(false);
    }
  };

  const quantity = cart.items.reduce((sum, item) => sum + item.quantity, 0);
  const categories = [...new Set(products.map(product => product.category).filter(Boolean))]
    .sort((a, b) => a.localeCompare(b, "pt-BR"));
  const visibleProducts = selectedCategory
    ? products.filter(product => product.category === selectedCategory)
    : products;
  const sortedProducts = [...visibleProducts].sort((left, right) => {
    if (sortBy === "price-ascending") return left.priceCents - right.priceCents;
    if (sortBy === "price-descending") return right.priceCents - left.priceCents;
    if (sortBy === "rating") {
      const ratingDifference = (Number(right.ratingAverage) || 0) - (Number(left.ratingAverage) || 0);
      if (ratingDifference !== 0) return ratingDifference;
      return (Number(right.ratingCount) || 0) - (Number(left.ratingCount) || 0);
    }
    return right.id - left.id;
  });
  const searchCategories = [...new Set(searchResults.map(product => product.category).filter(Boolean))]
    .sort((a, b) => a.localeCompare(b, "pt-BR"));
  const chartMonths = Array.from({ length: 6 }, (_, index) => {
    const date = new Date();
    date.setDate(1);
    date.setMonth(date.getMonth() - (5 - index));
    const key = `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, "0")}`;
    const month = sellerDashboard?.monthlyRevenue?.find(item => item.month === key);
    return { key, label: formatMonth(key), revenueCents: month?.revenueCents || 0 };
  });
  const maxRevenue = Math.max(1, ...chartMonths.map(month => month.revenueCents));

  const selectCategory = category => {
    setSelectedCategory(category);
    closeNavigation();
    document.querySelector("#produtos")?.scrollIntoView({ behavior: "smooth", block: "start" });
    document.querySelector("#products-title")?.focus({ preventScroll: true });
  };

  const closeNavigation = () => {
    setNavigationOpen(false);
    menuButton.current?.focus();
  };

  const openSearch = event => {
    if (searchOpen && !searchClosing) return;
    window.clearTimeout(searchCloseTimer.current);
    const bounds = event.currentTarget.getBoundingClientRect();
    const mobile = window.matchMedia("(max-width: 580px)").matches;
    const targetLeft = mobile ? 14 : 24;
    setSearchAnimationStyle({
      "--search-start-left": `${bounds.left}px`,
      "--search-start-top": `${bounds.top}px`,
      "--search-start-width": `${bounds.width}px`,
      "--search-target-left": `${targetLeft}px`,
      "--search-target-width": `${window.innerWidth - targetLeft * 2}px`
    });
    setSearchClosing(false);
    setSearchOpening(true);
    setSearchOpen(true);
    window.requestAnimationFrame(() => window.requestAnimationFrame(() => setSearchOpening(false)));
  };

  const closeSearch = () => {
    if (!searchOpen || searchClosing) return;
    setSearchOpening(false);
    setSearchClosing(true);
    const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    searchCloseTimer.current = window.setTimeout(() => {
      setSearchOpen(false);
      setSearchClosing(false);
      setSearchAnimationStyle({});
    }, reducedMotion ? 20 : 700);
  };

  const selectSearchCategory = category => {
    closeSearch();
    setSearch(category);
    setSelectedCategory("");
    loadProducts(category);
    document.querySelector("#produtos")?.scrollIntoView({ behavior: "smooth", block: "start" });
  };

  const selectSearchProduct = product => {
    openProductDetails(product);
  };

  useEffect(() => {
    if (!searchOpen) return undefined;
    const closeOnEscape = event => {
      if (event.key === "Escape") closeSearch();
    };
    document.addEventListener("keydown", closeOnEscape);
    return () => document.removeEventListener("keydown", closeOnEscape);
  }, [searchOpen]);

  useEffect(() => {
    if (!cartOpen) return undefined;
    const previousOverflow = document.body.style.overflow;
    document.body.style.overflow = "hidden";
    const closeOnEscape = event => {
      if (event.key === "Escape") setCartOpen(false);
    };
    document.addEventListener("keydown", closeOnEscape);
    return () => {
      document.body.style.overflow = previousOverflow;
      document.removeEventListener("keydown", closeOnEscape);
    };
  }, [cartOpen]);

  useEffect(() => {
    if (!navigationOpen) return undefined;
    const previousOverflow = document.body.style.overflow;
    document.body.style.overflow = "hidden";
    navigationCloseButton.current?.focus();
    const closeOnEscape = event => {
      if (event.key === "Escape") closeNavigation();
      if (event.key === "Tab") {
        const focusable = document.querySelectorAll("#store-navigation button:not(:disabled)");
        const first = focusable[0];
        const last = focusable[focusable.length - 1];
        if (event.shiftKey && document.activeElement === first) {
          event.preventDefault();
          last?.focus();
        } else if (!event.shiftKey && document.activeElement === last) {
          event.preventDefault();
          first?.focus();
        }
      }
    };
    document.addEventListener("keydown", closeOnEscape);
    return () => {
      document.body.style.overflow = previousOverflow;
      document.removeEventListener("keydown", closeOnEscape);
    };
  }, [navigationOpen]);

  return (
    <>
      <header className={`site-header${searchOpen ? " search-active" : ""}${searchOpening ? " search-opening" : ""}${searchClosing ? " search-closing" : ""}`}>
        <div className="header-main container">
          <a className="brand" href="/" aria-label="shope pago, página inicial">
            <span className="brand-mark"><StoreMark /></span><span>shope pago</span>
          </a>
          <button ref={menuButton} className={`menu-toggle${navigationOpen ? " is-active" : ""}`}
            onClick={() => setNavigationOpen(!navigationOpen)} type="button"
            aria-expanded={navigationOpen} aria-controls="store-navigation">
            <span className="menu-toggle-icon" aria-hidden="true"><i /><i /><i /></span>
            <span>Menu</span>
          </button>
          <form className="search" role="search" style={searchAnimationStyle} onFocus={openSearch} onSubmit={event => {
            event.preventDefault();
            setSelectedCategory("");
            closeSearch();
            loadProducts(search.trim());
          }} onBlur={event => {
            const nextFocus = event.relatedTarget;
            if (!event.currentTarget.contains(nextFocus) &&
                !document.querySelector(".search-results-panel")?.contains(nextFocus)) {
              closeSearch();
            }
          }}>
            <label className="sr-only" htmlFor="search-input">Buscar produtos</label>
            <span className="search-icon" aria-hidden="true">⌕</span>
            <input id="search-input" type="text" inputMode="search" placeholder="Buscar produtos publicados"
              autoComplete="off" value={search} onChange={event => {
                const value = event.target.value;
                setSearch(value);
                if (!value.trim()) {
                  setSelectedCategory("");
                  setSearchError("");
                  closeSearch();
                  loadProducts();
                }
              }} />
            {search && <button className="search-clear" onClick={() => {
              setSearch("");
              setSelectedCategory("");
              setSearchError("");
              loadProducts();
              closeSearch();
            }} type="button" aria-label="Limpar busca">×</button>}
            <button type="submit">Buscar</button>
          </form>
          {searchOpen && <section className={`search-results-panel${searchClosing ? " is-closing" : ""}`} aria-label="Sugestões de busca">
            {search.trim()
              ? <>
                <div className="search-suggestions-heading">
                  <span className="eyebrow">RESULTADOS PARA</span>
                  <strong>“{search.trim()}”</strong>
                </div>
                {searchCategories.length > 0 && <div className="search-category-options">
                  {searchCategories.map(category => <button key={category} onClick={() => selectSearchCategory(category)} type="button">
                    <span aria-hidden="true">◈</span>{category}
                  </button>)}
                </div>}
                {searchLoading
                  ? <p className="search-panel-message">Buscando produtos...</p>
                  : searchError
                    ? <p className="search-panel-message search-panel-error">{searchError}</p>
                    : searchResults.length
                      ? <div className="search-product-options">
                        {searchResults.slice(0, 6).map(product => <button key={product.id}
                          onClick={() => selectSearchProduct(product)} type="button">
                          <span className="search-result-image">
                            <ProductImage src={product.imageUrl} alt="" className="search-product-image"
                              fallbackClassName="search-product-placeholder" />
                          </span>
                          <span className="search-result-copy">
                            <strong>{product.name}</strong>
                            <small>{product.category} · {product.shippingCents
                              ? `Frete ${money(product.shippingCents)}` : "Frete grátis"} · {product.sellerName}</small>
                          </span>
                          <strong className="search-result-price">{money(product.priceCents)}</strong>
                        </button>)}
                        <button className="search-view-all" onClick={() => {
                          closeSearch();
                          setSelectedCategory("");
                          loadProducts(search.trim());
                          document.querySelector("#produtos")?.scrollIntoView({ behavior: "smooth", block: "start" });
                        }} type="button">Ver todos os resultados <span aria-hidden="true">→</span></button>
                      </div>
                      : <p className="search-panel-message">Nenhum produto encontrado. Tente outro nome ou categoria.</p>}
              </>
              : <div className="search-empty-state">
                <span className="eyebrow">ENCONTRE SEU PRÓXIMO ACHADO</span>
                <h2>O que você está procurando?</h2>
                <p>Digite um produto ou uma categoria para ver opções da loja.</p>
                {categories.length > 0 && <div className="search-category-options">
                  {categories.slice(0, 8).map(category => <button key={category} onClick={() => selectSearchCategory(category)} type="button">
                    <span aria-hidden="true">◈</span>{category}
                  </button>)}
                </div>}
              </div>}
          </section>}
          <div className="header-actions">
            <div className="account-area">
              <button className="header-button" type="button" aria-expanded={menuOpen}
                onClick={() => user ? setMenuOpen(!menuOpen) : openAccount()}>
                {user ? user.name : "Entrar / Criar conta"}
              </button>
              {user && menuOpen && <div className="account-menu">
                <strong>{user.name}</strong>
                <button onClick={openSellerForm} type="button">Painel do vendedor</button>
                <button onClick={openOrders} type="button">Meus pedidos</button>
                <button onClick={logout} type="button">Sair da conta</button>
              </div>}
            </div>
            <button className="header-button seller-button" onClick={openSellerForm} type="button">
              {sellerProfile ? "Acessar painel" : "Criar a minha venda"}
            </button>
            <button className="cart-button" onClick={() => user ? setCartOpen(true) : openAccount()}
              type="button" aria-label={`Abrir carrinho, ${quantity} itens`}>
              <span className="action-icon"><CartIcon /></span><span>Carrinho</span>
              <span className="cart-count">{quantity}</span>
            </button>
          </div>
        </div>
      </header>
      {searchOpen && <button className={`search-backdrop${searchClosing ? " is-closing" : ""}`} onClick={closeSearch} type="button" aria-label="Fechar sugestões de busca" />}

      <main className="container">
        <section className="hero" aria-label="Apresentação da loja">
          <div className="hero-copy">
            <span className="eyebrow">SHOPE PAGO · LOJA DE VENDEDORES</span>
            <h1>Encontre o que<br /><span>você procura.</span></h1>
            <p>Veja os produtos publicados por vendedores nesta loja. Para adicionar um produto ao carrinho, entre na sua conta.</p>
            <button className="primary-button" onClick={openSellerForm} type="button">
              {sellerProfile ? "Acessar painel" : "Criar a minha venda"} <span>→</span>
            </button>
          </div>
          <div className="hero-art" aria-hidden="true">
            <div className="hero-logo"><StoreMark className="hero-logo-image" /></div>
          </div>
        </section>

        <section className="products-section" id="produtos" aria-labelledby="products-title">
          <div className="section-heading products-heading">
            <div><span className="eyebrow">FEITO POR VENDEDORES</span>
              <h2 id="products-title" tabIndex="-1">Produtos publicados</h2></div>
            <span className="result-count" aria-live="polite">
              {!productsLoading && !productsError && `${visibleProducts.length} ${visibleProducts.length === 1 ? "produto" : "produtos"}`}
            </span>
          </div>
          {!productsLoading && !productsError && products.length > 1 && <div className="catalog-toolbar">
            <label htmlFor="product-sort">Ordenar por</label>
            <select id="product-sort" value={sortBy} onChange={event => setSortBy(event.target.value)}>
              <option value="newest">Mais recentes</option>
              <option value="price-ascending">Menor preço</option>
              <option value="price-descending">Maior preço</option>
              <option value="rating">Melhor avaliação</option>
            </select>
          </div>}
          {!productsLoading && !productsError && categories.length > 0 && <nav className="category-filters" aria-label="Filtrar por categoria">
            <button className={`category-chip${selectedCategory === "" ? " is-selected" : ""}`}
              onClick={() => setSelectedCategory("")} type="button" aria-pressed={selectedCategory === ""}>
              Todos <span>{products.length}</span></button>
            {categories.map(category => {
              const count = products.filter(product => product.category === category).length;
              return <button className={`category-chip${selectedCategory === category ? " is-selected" : ""}`}
                key={category} onClick={() => setSelectedCategory(category)} type="button"
                aria-pressed={selectedCategory === category}>{category} <span>{count}</span></button>;
            })}
          </nav>}
          {productsLoading
            ? <div className="product-grid"><div className="loading-state">Carregando produtos...</div></div>
            : productsError
              ? <div className="catalog-feedback" role="alert">
                <span aria-hidden="true">!</span>
                <div><strong>Não foi possível carregar o catálogo</strong><p>{productsError}</p></div>
                <button className="secondary-button" onClick={() => loadProducts(search)} type="button">Tentar novamente</button>
              </div>
              : sortedProducts.length
                ? <div className="product-grid" aria-live="polite">
                {sortedProducts.map(product => <ProductCard key={product.id} product={product} onAdd={addToCart}
                    onOpen={openProductDetails}
                    onRate={user ? rateProduct : null} />)}
                </div>
                : selectedCategory
                  ? <div className="empty-state">
                    <span aria-hidden="true">⌕</span>
                    <h3>Nenhum produto nesta categoria</h3>
                    <p>Escolha outra categoria ou veja todos os produtos publicados.</p>
                    <button className="secondary-button" onClick={() => setSelectedCategory("")} type="button">Ver todos os produtos</button>
                  </div>
                  : <div className="empty-state">
                  <span aria-hidden="true">▧</span>
                  <h3>Nenhum produto publicado</h3>
                  <p>Quando um vendedor cadastrar e publicar um produto, ele aparecerá aqui.</p>
                  <button className="secondary-button" onClick={openSellerForm} type="button">Publicar meu primeiro produto</button>
                </div>}
        </section>
      </main>

      <footer className="site-footer">
        <div className="container footer-main">
          <a className="brand footer-brand" href="/" aria-label="shope pago, página inicial">
            <span className="brand-mark"><StoreMark /></span><span>shope pago</span>
          </a>
          <span>Os produtos são publicados pelos vendedores cadastrados.</span>
          <span>{paymentEnabled
            ? "Checkout disponível com pagamento protegido pelo Mercado Pago."
            : "Pagamentos serão habilitados após configurar Mercado Pago e domínio HTTPS."}</span>
        </div>
      </footer>

      <dialog className="product-detail-dialog" ref={productDialog}
        onClose={() => setSelectedProduct(null)} aria-labelledby="product-detail-title">
        {selectedProduct && <>
          <div className="product-detail-topbar">
            <nav aria-label="Você está aqui">
              <button onClick={() => productDialog.current?.close()} type="button">Início</button>
              <span aria-hidden="true">›</span>
              <button onClick={() => {
                productDialog.current?.close();
                selectCategory(selectedProduct.category);
              }} type="button">{selectedProduct.category}</button>
              <span aria-hidden="true">›</span>
              <span>{selectedProduct.name}</span>
            </nav>
            <button className="icon-button" onClick={() => productDialog.current?.close()} type="button"
              aria-label="Fechar detalhes do produto">×</button>
          </div>
          <div className="product-detail-layout">
            <div className="product-detail-gallery">
              <div className="product-detail-main-image">
                {selectedProduct.imageUrl
                  ? <ProductImage src={selectedProduct.imageUrl} alt={selectedProduct.name}
                      className="product-detail-image" fallbackClassName="product-placeholder" />
                  : <span aria-hidden="true">▧</span>}
              </div>
            </div>
            <section className="product-detail-information">
              <span className="eyebrow">PRODUTO PUBLICADO POR VENDEDOR</span>
              <h1 id="product-detail-title">{selectedProduct.name}</h1>
              <ProductRating product={selectedProduct} onRate={user ? rateProduct : null} />
              <p className="product-detail-seller">Vendido por <strong>{selectedProduct.sellerName}</strong></p>
              <hr />
              <h2>Descrição do produto</h2>
              <p className="product-detail-description">{selectedProduct.description || "O vendedor ainda não adicionou uma descrição."}</p>
              <section className="product-detail-specifications" aria-label="Detalhes do produto">
                <h2>Detalhes do produto</h2>
                <dl>
                  <div><dt>Categoria</dt><dd>{selectedProduct.category}</dd></div>
                  <div><dt>Vendedor</dt><dd>{selectedProduct.sellerName}</dd></div>
                  <div><dt>Avaliações</dt><dd>{selectedProduct.ratingCount || 0}</dd></div>
                </dl>
              </section>
            </section>
            <aside className="product-detail-purchase">
              <span className="eyebrow">PREÇO</span>
              <strong className="product-detail-price">{money(selectedProduct.priceCents)}</strong>
              <p>Preço informado pelo vendedor.</p>
              <p>Frete por unidade: {selectedProduct.shippingCents
                ? money(selectedProduct.shippingCents) : "grátis"}</p>
              <button className="checkout-button" onClick={() => {
                addToCart(selectedProduct.id);
                productDialog.current?.close();
              }} type="button">Adicionar ao carrinho</button>
              <p className="payment-notice">O frete é definido pelo vendedor e informado antes do pagamento.</p>
              <p className="product-detail-seller-line">Vendido por <strong>{selectedProduct.sellerName}</strong></p>
            </aside>
          </div>
        </>}
      </dialog>

      <div className={`navigation-backdrop${navigationOpen ? " is-visible" : ""}`}
        hidden={!navigationOpen} onClick={closeNavigation} />
      <aside id="store-navigation" className={`store-navigation${navigationOpen ? " is-open" : ""}`}
        role="dialog" aria-modal="true" aria-hidden={!navigationOpen} inert={!navigationOpen} aria-label="Menu da loja">
        <div className="navigation-heading">
          <div className="navigation-brand"><span className="brand-mark"><StoreMark /></span>
            <span>shope pago</span></div>
          <button ref={navigationCloseButton} className="icon-button" onClick={closeNavigation} type="button" aria-label="Fechar menu">×</button>
        </div>
        <p className="navigation-caption">NAVEGAÇÃO</p>
        <nav className="navigation-links" aria-label="Principal">
          <button onClick={() => { setSelectedCategory(""); closeNavigation(); window.scrollTo({ top: 0, behavior: "smooth" }); }} type="button">
            Início
          </button>
          <button onClick={() => selectCategory("")} type="button">Todos os produtos</button>
        </nav>
        <p className="navigation-caption">CATEGORIAS PUBLICADAS</p>
        {categories.length
          ? <nav className="navigation-links" aria-label="Categorias">
            {categories.map(category => <button key={category} onClick={() => selectCategory(category)} type="button">
              <span aria-hidden="true">◈</span>{category}
              <small>{products.filter(product => product.category === category).length}</small>
            </button>)}
          </nav>
          : <p className="navigation-empty">As categorias aparecerão aqui quando vendedores publicarem produtos.</p>}
        <div className="navigation-spacer" />
        <div className="navigation-actions">
          <button className="navigation-sell" onClick={() => { setNavigationOpen(false); openSellerForm(); }} type="button">
            {sellerProfile ? "Acessar painel" : "Criar a minha venda"}
          </button>
          <button onClick={() => { closeNavigation(); user ? openSellerForm() : openAccount("login", { type: "sell" }); }} type="button">
            <span aria-hidden="true">♙</span>{user ? "Minha conta e anúncios" : "Entrar ou criar conta"}
          </button>
          {user && <button onClick={() => { closeNavigation(); openOrders(); }} type="button">
            <span aria-hidden="true">▤</span>Meus pedidos
          </button>}
          <button onClick={() => { closeNavigation(); user ? setCartOpen(true) : openAccount(); }} type="button">
            <span className="navigation-cart-icon"><CartIcon /></span>Carrinho <small>{quantity}</small>
          </button>
        </div>
        <p className="navigation-footer">Produtos publicados pelos vendedores da shope pago.</p>
      </aside>

      <div className={`overlay${cartOpen ? " is-visible" : ""}`} hidden={!cartOpen} onClick={() => setCartOpen(false)} />
      <aside className={`cart-panel${cartOpen ? " is-open" : ""}`} aria-labelledby="cart-title"
        aria-hidden={!cartOpen} inert={!cartOpen}>
        <div className="cart-header">
          <div><span className="eyebrow">SUA CONTA</span><h2 id="cart-title">Carrinho <span>({quantity})</span></h2></div>
          <button className="icon-button" onClick={() => setCartOpen(false)} type="button" aria-label="Fechar carrinho">×</button>
        </div>
        <div className="cart-items">{cart.items.map(item => (
          <article className="cart-line" key={item.id}>
            <div className="cart-thumb">{item.imageUrl
              ? <ProductImage src={item.imageUrl} alt="" className="cart-product-image"
                  fallbackClassName="cart-product-placeholder" />
              : <span aria-hidden="true">▧</span>}</div>
            <div className="cart-line-info">
              <strong>{item.name}</strong><small>{money(item.priceCents)} cada</small>
              <small>Frete: {item.shippingCents ? `${money(item.shippingCents)} por unidade` : "grátis"}</small>
              <div className="quantity-control">
                <button onClick={() => changeCart(item.id, true)} type="button" aria-label="Remover produto">−</button>
                <span>{item.quantity}</span>
                <button onClick={() => changeCart(item.id)} type="button" aria-label="Adicionar mais um">＋</button>
              </div>
            </div>
            <strong className="cart-line-price">
              {money((item.priceCents + item.shippingCents) * item.quantity)}
              <small>subtotal c/ frete</small>
            </strong>
          </article>
        ))}</div>
        {!cart.items.length && <div className="cart-empty">
          <span className="cart-empty-icon"><CartIcon /></span><strong>Seu carrinho está vazio</strong>
          <p>{user ? "Adicione produtos publicados para continuar." : "Entre na sua conta para adicionar produtos."}</p>
        </div>}
        {!!cart.items.length && <div className="cart-footer">
          <div className="cart-total"><span>Total com frete informado</span><strong>{money(cart.totalCents)}</strong></div>
          <p className="payment-notice">{paymentEnabled
            ? "Você informará o endereço e pagará com segurança no Mercado Pago."
            : "O checkout estará disponível depois que o administrador configurar o Mercado Pago."}</p>
          <button className="checkout-button" onClick={openCheckout} disabled={!paymentEnabled} type="button">
            Continuar para entrega e pagamento
          </button>
        </div>}
      </aside>

      <dialog className="seller-dialog checkout-dialog" ref={checkoutDialog}>
        <form onSubmit={registerOrder}>
          <div className="dialog-heading">
            <div><span className="eyebrow">FINALIZAR COMPRA</span><h2>Endereço de entrega</h2></div>
            <button className="icon-button" onClick={() => checkoutDialog.current?.close()} type="button" aria-label="Fechar">×</button>
          </div>
          {!paymentEnabled && <p className="payment-notice">O administrador precisa configurar Mercado Pago, webhook e domínio HTTPS antes de receber pagamentos.</p>}
          <div className="checkout-address-fields">
            <label>Nome de quem recebe<input name="recipientName" required minLength="2" maxLength="100" autoComplete="name" /></label>
            <div className="form-row">
              <label>Telefone<input name="recipientPhone" type="tel" required minLength="10" maxLength="20" autoComplete="tel" placeholder="(11) 99999-9999" /></label>
              <label>CEP<input name="postalCode" inputMode="numeric" required minLength="8" maxLength="9" autoComplete="postal-code" placeholder="00000-000" /></label>
            </div>
            <div className="form-row">
              <label>Estado<input name="state" required minLength="2" maxLength="80" autoComplete="address-level1" /></label>
              <label>Cidade<input name="city" required minLength="2" maxLength="100" autoComplete="address-level2" /></label>
            </div>
            <div className="form-row">
              <label>Rua/avenida<input name="street" required minLength="2" maxLength="160" autoComplete="street-address" /></label>
              <label>Número<input name="addressNumber" required maxLength="20" /></label>
            </div>
            <label>Complemento (opcional)<input name="complement" maxLength="100" /></label>
          </div>
          <div className="cart-total checkout-grand-total">
            <span>Total incluindo frete</span><strong>{money(cart.totalCents)}</strong>
          </div>
          <p className="payment-notice">O valor do frete é definido pelo vendedor para cada unidade. A loja envia o endereço necessário para a entrega.</p>
          <button className="checkout-button" type="submit" disabled={!paymentEnabled || submittingCheckout}>
            {submittingCheckout ? "Conectando ao Mercado Pago..." : "Ir para pagamento"}
          </button>
        </form>
      </dialog>

      <dialog className="account-dialog" ref={accountDialog}>
        <div className="auth-dialog-content">
          <div className="auth-dialog-heading">
            <div>
              <span className="eyebrow">SUA CONTA</span>
              <h2>{pendingAction?.type === "sell"
                ? authMode === "login" ? "Entre para começar a vender" : "Crie sua conta de vendedor"
                : authMode === "login" ? "Boas-vindas de volta" : "Crie sua conta"}</h2>
              <p>{pendingAction?.type === "sell"
                ? "Depois do acesso, vamos fazer seu cadastro de vendedor em algumas etapas."
                : authMode === "login" ? "Entre para comprar e acompanhar seus pedidos." : "Cadastre-se para comprar e publicar seus produtos."}</p>
            </div>
            <button className="icon-button" onClick={() => accountDialog.current?.close()} type="button" aria-label="Fechar">×</button>
          </div>
          <div className="auth-tabs" role="tablist" aria-label="Acesso à conta">
            <button className={`auth-tab${authMode === "login" ? " is-active" : ""}`} onClick={() => setAuthMode("login")}
              type="button" role="tab" aria-selected={authMode === "login"}>Entrar</button>
            <button className={`auth-tab${authMode === "register" ? " is-active" : ""}`} onClick={() => setAuthMode("register")}
              type="button" role="tab" aria-selected={authMode === "register"}>Criar conta</button>
          </div>
          {authMode === "login"
            ? <form className="auth-form" onSubmit={event => submitAuth(event, "/api/login")}>
              <label>E-mail<input name="email" type="email" autoComplete="email" required maxLength="254" placeholder="voce@email.com" /></label>
              <label>Senha<input name="password" type="password" autoComplete="current-password" required maxLength="256" placeholder="Sua senha" /></label>
              <button className="checkout-button" disabled={submittingAuth} type="submit">{submittingAuth ? "Entrando..." : "Entrar"}</button>
            </form>
            : <form className="auth-form" onSubmit={event => submitAuth(event, "/api/register")}>
              <label>Nome<input name="name" autoComplete="name" required minLength="2" maxLength="80" placeholder="Seu nome" /></label>
              <label>E-mail<input name="email" type="email" autoComplete="email" required maxLength="254" placeholder="voce@email.com" /></label>
              <label>Senha<input name="password" type="password" autoComplete="new-password" required minLength="10" maxLength="256" placeholder="Pelo menos 10 caracteres" /></label>
              <button className="checkout-button" disabled={submittingAuth} type="submit">{submittingAuth ? "Criando..." : "Criar conta"}</button>
            </form>}
          <div className="auth-google-section">
            <span className="auth-google-divider">ou</span>
            <button className="google-login-button" onClick={startGoogleLogin} disabled={!googleEnabled} type="button"
              title={googleEnabled ? "Entrar com sua conta Google" : "O administrador ainda precisa configurar o login Google"}>
              <svg className="google-mark" viewBox="0 0 24 24" aria-hidden="true">
                <path fill="#4285F4" d="M23.49 12.27c0-.79-.07-1.54-.2-2.27H12v4.51h6.44a5.52 5.52 0 0 1-2.39 3.62v3.01h3.87c2.26-2.08 3.57-5.15 3.57-8.87Z" />
                <path fill="#34A853" d="M12 24c3.24 0 5.96-1.08 7.95-2.92l-3.87-3.01c-1.08.72-2.46 1.15-4.08 1.15-3.14 0-5.8-2.12-6.75-4.97H1.25v3.1A12 12 0 0 0 12 24Z" />
                <path fill="#FBBC05" d="M5.25 14.25a7.2 7.2 0 0 1 0-4.5v-3.1h-4A12 12 0 0 0 0 12c0 1.93.46 3.75 1.25 5.35l4-3.1Z" />
                <path fill="#EA4335" d="M12 4.78c1.77 0 3.36.61 4.61 1.82l3.45-3.45C17.95 1.17 15.24 0 12 0A12 12 0 0 0 1.25 6.65l4 3.1C6.2 6.9 8.86 4.78 12 4.78Z" />
              </svg>
              {googleEnabled ? "Continuar com Google" : "Entrar com Google"}
            </button>
            {!googleEnabled && <p className="google-unavailable">O login Google será habilitado quando a loja configurar o serviço.</p>}
          </div>
          <p className="form-message" role="status" aria-live="polite">{authMessage}</p>
          <p className="auth-terms">Ao continuar, você concorda em usar a shope pago de forma responsável.</p>
        </div>
        <aside className="auth-visual" aria-label="Sacola de compras da shope pago">
          <div className="auth-visual-copy">
            <span className="auth-visual-kicker">BEM-VINDO À SHOPE PAGO</span>
            <h3>Seu próximo achado<br />está aqui.</h3>
          </div>
          <img className="shopping-illustration" src={loginShoppingBag}
            alt="Sacola de compras em uma loja iluminada" />
          <StoreMark className="auth-bag-logo" />
        </aside>
      </dialog>

      <dialog className="seller-dialog onboarding-dialog" ref={onboardingDialog}>
        <form onSubmit={submitSellerStep}>
          <div className="dialog-heading">
            <div><span className="eyebrow">CADASTRO DE VENDEDOR · {onboardingStep + 1} DE 4</span>
              <h2>{["Vamos começar", "Sua data de nascimento", "Como falar com você?", "O que você vende?"][onboardingStep]}</h2>
            </div>
            <button className="icon-button" onClick={() => onboardingDialog.current?.close()} type="button" aria-label="Fechar cadastro">×</button>
          </div>
          <div className="onboarding-progress" aria-hidden="true">
            <span style={{ width: `${((onboardingStep + 1) / 4) * 100}%` }} />
          </div>
          <p className="form-intro">
            {onboardingStep === 0 && `Conta: ${user?.email}. Precisamos validar seu CPF para criar seu perfil de vendedor.`}
            {onboardingStep === 1 && "Informe sua data de nascimento. A data não pode ser futura."}
            {onboardingStep === 2 && "Seu telefone será preenchido automaticamente quando você anunciar um produto."}
            {onboardingStep === 3 && "Escolha a área que mais combina com os produtos que você vende."}
          </p>
          {onboardingStep === 0 && <label>CPF
            <input autoFocus inputMode="numeric" autoComplete="off" maxLength="14" placeholder="000.000.000-00"
              value={onboardingDraft.cpf} onChange={event => setOnboardingDraft({ ...onboardingDraft, cpf: formatCpf(event.target.value) })} required />
          </label>}
          {onboardingStep === 1 && <label>Data de nascimento
            <input autoFocus type="date" autoComplete="bday" min="1900-01-01" max={new Date().toISOString().slice(0, 10)}
              value={onboardingDraft.birthDate} onChange={event => setOnboardingDraft({ ...onboardingDraft, birthDate: event.target.value })} required />
          </label>}
          {onboardingStep === 2 && <label>Telefone com DDD
            <input autoFocus type="tel" autoComplete="tel" inputMode="tel" maxLength="20" placeholder="(11) 99999-9999"
              value={onboardingDraft.phone} onChange={event => setOnboardingDraft({ ...onboardingDraft, phone: formatPhone(event.target.value) })} required />
          </label>}
          {onboardingStep === 3 && <label>O que você mais vende?
            <select autoFocus value={onboardingDraft.interests}
              onChange={event => setOnboardingDraft({ ...onboardingDraft, interests: event.target.value })} required>
              <option value="">Selecione uma categoria</option>
              {["Tecnologia", "Casa e decoração", "Moda e acessórios", "Beleza e saúde", "Esportes e lazer", "Brinquedos", "Alimentos", "Outros"]
                .map(category => <option key={category} value={category}>{category}</option>)}
            </select>
          </label>}
          <p className="form-message" role="status" aria-live="polite">{onboardingMessage}</p>
          <div className="onboarding-actions">
            {onboardingStep > 0 && <button className="secondary-button" onClick={() => {
              setOnboardingMessage("");
              setOnboardingStep(onboardingStep - 1);
            }} type="button">Voltar</button>}
            <button className="checkout-button" disabled={submittingSellerProfile} type="submit">
              {submittingSellerProfile ? "Salvando..." : onboardingStep === 3 ? "Concluir cadastro" : "Continuar"}
            </button>
          </div>
          <p className="privacy-note">Seu CPF é validado para este cadastro e não é armazenado pela loja.</p>
        </form>
      </dialog>

      <dialog className="seller-dialog" ref={sellerDialog}>
        <form ref={productForm} onSubmit={submitProduct}>
          <div className="dialog-heading">
            <div><span className="eyebrow">ÁREA DO VENDEDOR</span><h2>Publicar produto</h2></div>
            <button className="icon-button" onClick={() => sellerDialog.current?.close()} type="button" aria-label="Fechar">×</button>
          </div>
          <p className="form-intro">Informe os dados do produto. Ele ficará visível no catálogo público.</p>
          <label>Telefone para contato
            <input type="tel" value={sellerProfile?.phone || ""} readOnly />
          </label>
          <label>Nome do produto<input name="name" required minLength="2" maxLength="120" placeholder="Ex.: Cadeira de escritório" /></label>
          <label>Descrição<textarea name="description" required minLength="2" maxLength="2000" rows="4" placeholder="Detalhes do produto" /></label>
          <div className="form-row">
            <label>Categoria<input name="category" required minLength="2" maxLength="60" placeholder="Ex.: Casa" /></label>
            <label>Preço (R$)<input name="price" type="number" min="0.01" max="1000000000" step="0.01" required placeholder="0,00" /></label>
          </div>
          <label>Frete por unidade (R$)
            <input name="shipping" type="number" min="0" max="1000000000" step="0.01" required defaultValue="0" />
          </label>
          <p className="payment-notice">Informe o valor cobrado para enviar uma unidade. Use zero para frete grátis.</p>
          <label>Imagem do produto · link HTTPS
            <input name="imageUrl" type="url" required placeholder="https://..." /></label>
          <p className="form-message" role="status" aria-live="polite">{productMessage}</p>
          <button className="checkout-button" disabled={submittingProduct} type="submit">
            {submittingProduct ? "Publicando..." : "Vender produto"}
          </button>
        </form>
      </dialog>

      <dialog className="seller-dialog" ref={productsDialog}>
        <div className="dialog-heading">
          <div><span className="eyebrow">ÁREA DO VENDEDOR</span><h2>Painel do vendedor</h2></div>
          <button className="icon-button" onClick={() => productsDialog.current?.close()} type="button" aria-label="Fechar">×</button>
        </div>
        {manageLoading ? <p className="loading-state">Carregando painel...</p>
          : manageMessage ? <p className="form-message">{manageMessage}</p>
            : sellerDashboard && <div className="seller-dashboard">
              <div className="dashboard-stats">
                <article><span>Produtos à venda</span><strong>{sellerDashboard.activeProducts}</strong></article>
                <article><span>Pedidos recebidos</span><strong>{sellerDashboard.ordersReceived}</strong></article>
                <article><span>Dinheiro recebido</span><strong>{money(sellerDashboard.revenueCents)}</strong></article>
              </div>
              <section className="revenue-chart" aria-labelledby="revenue-chart-title">
                <div className="chart-heading"><div><span className="eyebrow">ÚLTIMOS 6 MESES</span>
                  <h3 id="revenue-chart-title">Receita confirmada</h3></div>
                  <strong>{money(sellerDashboard.revenueCents)}</strong>
                </div>
                <div className="chart-bars" role="img" aria-label="Gráfico de receita confirmada nos últimos seis meses">
                  {chartMonths.map(month => <div className="chart-column" key={month.key}>
                    <span className="chart-value">{money(month.revenueCents)}</span>
                    <span className="chart-bar-track"><span style={{ height: `${Math.max(4, (month.revenueCents / maxRevenue) * 100)}%` }} /></span>
                    <span className="chart-label">{month.label}</span>
                  </div>)}
                </div>
                <p className="payment-notice">O painel atualiza o valor recebido somente depois da confirmação do Mercado Pago.</p>
              </section>
              <section className="seller-orders-section">
                <h3>Pedidos e entregas</h3>
                {sellerOrders.length === 0
                  ? <p>Os pedidos dos clientes aparecerão aqui.</p>
                  : sellerOrders.map(order => <article className="seller-order-card" key={order.id}>
                    <div className="seller-order-heading">
                      <strong>Pedido #{order.id} · {order.buyerName}</strong>
                      <span>{orderStatusLabel(order.status)}</span>
                    </div>
                    <p>{order.products}</p>
                    <small>Frete: {money(order.shippingCents)} · Criado em {new Date(`${order.createdAt}Z`).toLocaleString("pt-BR")}</small>
                    {order.status === "paid" && <address>
                      {order.recipientName} · {order.recipientPhone}<br />
                      {order.street}, {order.addressNumber}{order.complement ? `, ${order.complement}` : ""}<br />
                      {order.city} - {order.state}, CEP {order.postalCode}
                    </address>}
                    {order.trackingCode && <small>Rastreio: {order.trackingCode}</small>}
                    {order.status === "paid" && order.shipmentStatus === "awaiting_shipment" && <div className="seller-shipment-actions">
                      <label>Código de rastreio (opcional)
                        <input maxLength="80" value={sellerTrackingCodes[order.id] ?? order.trackingCode}
                          onChange={event => setSellerTrackingCodes(current => ({
                            ...current, [order.id]: event.target.value
                          }))} />
                      </label>
                      <button className="secondary-button" onClick={() => updateSellerOrder(order.id, "shipped")} type="button">
                        Marcar como enviado
                      </button>
                    </div>}
                    {order.status === "paid" && order.shipmentStatus === "shipped" && <button className="secondary-button"
                      onClick={() => updateSellerOrder(order.id, "delivered")} type="button">
                      Confirmar entrega
                    </button>}
                    {order.shipmentStatus === "delivered" && <strong>Entrega concluída</strong>}
                    {order.status === "awaiting_payment" && <small>Os dados de entrega e o envio serão liberados após a aprovação do pagamento.</small>}
                  </article>)}
              </section>
              <div className="dashboard-products-heading">
                <h3>Produtos à venda</h3>
                <button className="secondary-button" onClick={openProductForm} type="button">＋ Vender produto</button>
              </div>
            </div>}
        <div className="manage-list">
          {!manageLoading && !manageMessage && (myProducts.length === 0
            ? <p>Você ainda não publicou produtos.</p>
            : myProducts.map(product => <article className="manage-item" key={product.id}>
                  <div className="manage-product-image">
                    {product.imageUrl
                      ? <ProductImage src={product.imageUrl} alt="" className="manage-product-image-content"
                          fallbackClassName="manage-product-placeholder" />
                      : <span aria-hidden="true">▧</span>}
                  </div>
                  <div className="manage-product-copy"><strong>{product.name}</strong>
                    <small>{money(product.priceCents)} · {product.category} · Frete {money(product.shippingCents)}</small>
                    <ProductRating product={product} /></div>
                  <button className="remove-button" onClick={() => removeProduct(product.id)} type="button">Remover anúncio</button>
                </article>))}
        </div>
      </dialog>

      <dialog className="seller-dialog" ref={ordersDialog}>
        <div className="dialog-heading">
          <div><span className="eyebrow">SUA CONTA</span><h2>Meus pedidos</h2></div>
          <button className="icon-button" onClick={() => ordersDialog.current?.close()} type="button" aria-label="Fechar">×</button>
        </div>
        <div className="manage-list">
          {manageLoading ? <p className="loading-state">Carregando pedidos...</p>
            : manageMessage ? <p className="form-message">{manageMessage}</p>
              : orders.length === 0 ? <p>Você ainda não registrou pedidos.</p>
                : orders.map(order => <article className="manage-item" key={order.id}>
                  <div><strong>Pedido #{order.id}</strong>
                    <small>{new Date(`${order.createdAt}Z`).toLocaleString("pt-BR")}</small>
                    {order.status !== "refunded" && order.shipments?.map((shipment, index) => <small key={`${order.id}-${index}`}>
                      {shipment.sellerName}: {orderStatusLabel(shipment.status)}
                      {shipment.trackingCode ? ` · rastreio ${shipment.trackingCode}` : ""}
                    </small>)}
                  </div>
                  <div className="order-status"><strong>{money(order.totalCents)}</strong>
                    <small>{orderStatusLabel(order.status)}</small>
                    {order.paymentStatus !== "approved" && <small>Pagamento: {orderStatusLabel(order.paymentStatus)}</small>}
                    {order.status === "awaiting_payment" && order.checkoutUrl &&
                      <button className="secondary-button" onClick={() => window.location.assign(order.checkoutUrl)} type="button">
                        Continuar pagamento
                      </button>}
                  </div>
                </article>)}
        </div>
        <p className="payment-notice">O status do pagamento vem do Mercado Pago; o vendedor atualiza cada etapa da entrega.</p>
      </dialog>

      <div className={`toast${toast ? " is-visible" : ""}`} role="status" aria-live="polite">{toast}</div>
    </>
  );
}

export default App;
