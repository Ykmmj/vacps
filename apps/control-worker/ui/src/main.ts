import { mount } from 'svelte';

import './app.css';
import 'flag-icons/css/flag-icons.min.css';
import App from './App.svelte';

const target = document.getElementById('app');
if (!target) throw new Error('App mount target was not found.');

mount(App, { target });
