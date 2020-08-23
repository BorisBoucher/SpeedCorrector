
freqTable = [
	[0, 0],
	[35.46, 29.55],
	[62.48, 56.23],
	[106.6, 91.68],
];

def convertPeriod(inputPeriod):
	
	inputFreq = 1.0 / inputPeriod
	
	index = 0;
	
	if inputFreq < freqTable[1][0]:
		index = 1
	elif inputFreq < freqTable[2][0]:
		index = 2
	else:
		index = 3
	
	di = freqTable[index][0] - freqTable[index-1][0]
	doo = freqTable[index][1] - freqTable[index-1][1]
	outFreq = (inputFreq - freqTable[index-1][0]) / di * doo
	outFreq = outFreq + freqTable[index-1][1]
	
	# And convert to output period
	outputPeriod = (1.0 / outFreq) 
	
	return outputPeriod
	

v = 1

lastPout = 1000
while v < 300.0:

	f = v * 0.73
	pin = 1.0 / f
	pout = convertPeriod(pin)
	
	print "{:4.1f}".format(1/pout/0.73), "km/h; ", "{:1.6f}".format(pin), " ; ", "{:1.6f}".format(pout), ";{:1.6f}".format(lastPout - pout)
	if pout > lastPout:
		print "****"
	
	v += 1.0
	lastPout = pout
	
		
	