import pandas as pd
import matplotlib.pyplot as plt

df = pd.read_csv('build/test/perf.csv', header=None, names=['ix', 'tag', 'time'])
df = df.pivot(index='ix', columns='tag', values='time')
plt.figure()
axes = df.boxplot(figsize=(5,5), fontsize='8', grid=False)
plt.ylim(0, 500000000)
plt.savefig('build/test/perf.png', dpi=300)
