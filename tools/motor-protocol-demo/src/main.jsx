import React from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App.jsx";
import { DeviceApp } from "./DeviceApp.jsx";
const RootApp = import.meta.env.VITE_DEVICE === "1" || new URLSearchParams(location.search).has("device") ? DeviceApp : App;
import "./styles.css";
import "./device.css";

createRoot(document.getElementById("root")).render(
  <React.StrictMode>
    <RootApp />
  </React.StrictMode>,
);
