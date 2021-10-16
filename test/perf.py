import pandas as pd
import matplotlib.pyplot as plt

df = pd.read_csv('build/test/perf.csv', header=None, names=['ix', 'tag', 'time'])
df = df.pivot(index='ix', columns='tag', values='time')
plt.figure()
boxplot = df.boxplot(figsize=(5,5), fontsize='8', grid=False)
plt.savefig('build/test/perf.png', dpi=300)
