// Package metrics 提供最小化 /metrics HTTP handler（Prometheus 文本格式）。
// 自实现避免引入 client_golang 依赖（沙箱无外网拉不到）。
// 生产可换 prometheus/client_golang。
package metrics

import (
	"fmt"
	"net/http"
	"sort"
	"strings"
	"sync"
)

var (
	mu       sync.Mutex
	counters = map[string]float64{}
	gauges   = map[string]float64{}
	order    []string
)

// IncCounter 原子 +1。
func IncCounter(name string) { AddCounter(name, 1) }

// AddCounter 原子 +v。
func AddCounter(name string, v float64) {
	mu.Lock()
	defer mu.Unlock()
	if _, ok := counters[name]; !ok {
		order = append(order, name)
	}
	counters[name] += v
}

// SetGauge 设置当前值。
func SetGauge(name string, v float64) {
	mu.Lock()
	defer mu.Unlock()
	if _, ok := gauges[name]; !ok {
		// order 混合 counter+gauge，保持插入序
		found := false
		for _, k := range order {
			if k == name {
				found = true
				break
			}
		}
		if !found {
			order = append(order, name)
		}
	}
	gauges[name] = v
}

// Handler 返回 /metrics 的 http.Handler。
func Handler() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		keys := make([]string, len(order))
		copy(keys, order)
		sort.Strings(keys)
		var sb strings.Builder
		for _, k := range keys {
			if v, ok := counters[k]; ok {
				sb.WriteString(fmt.Sprintf("# TYPE %s counter\n%s %v\n", k, k, v))
			}
			if v, ok := gauges[k]; ok {
				sb.WriteString(fmt.Sprintf("# TYPE %s gauge\n%s %v\n", k, k, v))
			}
		}
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		fmt.Fprint(w, sb.String())
	})
}
