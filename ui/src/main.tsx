import React from "react";
import ReactDOM from "react-dom/client";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import App from "./App";
import DeploymentAuthBoundary from "./components/DeploymentAuthBoundary";
import "highlight.js/styles/github-dark.css";
import "./styles.css";

const qc = new QueryClient();

ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <QueryClientProvider client={qc}>
      <DeploymentAuthBoundary><App /></DeploymentAuthBoundary>
    </QueryClientProvider>
  </React.StrictMode>,
);
