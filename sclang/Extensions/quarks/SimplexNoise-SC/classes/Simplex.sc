//
// Simplex noise class (in 2D and 3D), based on the example Java code
// (in the public domain) by Stefan Gustavson, with optimizations by
// Peter Eastman.
//
// Paper: http://staffwww.itn.liu.se/~stegu/simplexnoise/simplexnoise.pdf
// Example code: http://weber.itn.liu.se/~stegu/simplexnoise/SimplexNoise.java
//
// Port to SuperCollider and additions by Glen Fraser in 2020 (adding new
// helper methods `periodic`, `fBm2` and `fBm3`).
//

/*
 * A speed-improved simplex noise algorithm for 2D, 3D and 4D in Java.
 *
 * Based on example code by Stefan Gustavson (stegu@itn.liu.se).
 * Optimisations by Peter Eastman (peastman@drizzle.stanford.edu).
 * Better rank ordering method for 4D by Stefan Gustavson in 2012.
 *
 * This could be speeded up even further, but it's useful as it is.
 *
 * Version 2012-03-09
 *
 * This code was placed in the public domain by its original author,
 * Stefan Gustavson. You may use it as you see fit, but
 * attribution is appreciated.
 *
 */

Simplex {
	classvar grad3;
	classvar grad4;
	classvar perm;
	classvar permMod12;

	// Skewing and unskewing factors for 2, 3, and 4 dimensions
	const kF2 = 0.36602540378444; // 0.5*(sqrt(3.0)-1.0);
	const kG2 = 0.21132486540519; // (3.0-sqrt(3.0))/6.0;
	const kF3 = 0.33333333333333; // 1.0/3.0;
	const kG3 = 0.16666666666667; // 1.0/6.0;
	const kF4 = 0.30901699437495; // (sqrt(5.0)-1.0)/4.0;
	const kG4 = 0.13819660112501; // (5.0-sqrt(5.0))/20.0;

	*initClass {
		var p = [
			151,160,137,91,90,15,
			131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
			190, 6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,
			88,237,149,56,87,174,20,125,136,171,168, 68,175,74,165,71,134,139,48,27,166,
			77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,
			102,143,54, 65,25,63,161, 1,216,80,73,209,76,132,187,208, 89,18,169,200,196,
			135,130,116,188,159,86,164,100,109,198,173,186, 3,64,52,217,226,250,124,123,
			5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,
			223,183,170,213,119,248,152, 2,44,154,163, 70,221,153,101,155,167, 43,172,9,
			129,22,39,253, 19,98,108,110,79,113,224,232,178,185, 112,104,218,246,97,228,
			251,34,242,193,238,210,144,12,191,179,162,241, 81,51,145,235,249,14,239,107,
			49,192,214, 31,181,199,106,157,184, 84,204,176,115,121,50,45,127, 4,150,254,
			138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
		];
		grad3 = [
			SimplexGrad(1,1,0), SimplexGrad(-1,1,0), SimplexGrad(1,-1,0),
			SimplexGrad(-1,-1,0), SimplexGrad(1,0,1), SimplexGrad(-1,0,1),
			SimplexGrad(1,0,-1), SimplexGrad(-1,0,-1), SimplexGrad(0,1,1),
			SimplexGrad(0,-1,1), SimplexGrad(0,1,-1), SimplexGrad(0,-1,-1)
		];
		grad4 = [
			SimplexGrad(0,1,1,1), SimplexGrad(0,1,1,-1), SimplexGrad(0,1,-1,1),
			SimplexGrad(0,1,-1,-1), SimplexGrad(0,-1,1,1), SimplexGrad(0,-1,1,-1),
			SimplexGrad(0,-1,-1,1), SimplexGrad(0,-1,-1,-1), SimplexGrad(1,0,1,1),
			SimplexGrad(1,0,1,-1), SimplexGrad(1,0,-1,1), SimplexGrad(1,0,-1,-1),
			SimplexGrad(-1,0,1,1), SimplexGrad(-1,0,1,-1), SimplexGrad(-1,0,-1,1),
			SimplexGrad(-1,0,-1,-1), SimplexGrad(1,1,0,1), SimplexGrad(1,1,0,-1),
			SimplexGrad(1,-1,0,1), SimplexGrad(1,-1,0,-1), SimplexGrad(-1,1,0,1),
			SimplexGrad(-1,1,0,-1), SimplexGrad(-1,-1,0,1), SimplexGrad(-1,-1,0,-1),
			SimplexGrad(1,1,1,0), SimplexGrad(1,1,-1,0), SimplexGrad(1,-1,1,0),
			SimplexGrad(1,-1,-1,0), SimplexGrad(-1,1,1,0), SimplexGrad(-1,1,-1,0),
			SimplexGrad(-1,-1,1,0), SimplexGrad(-1,-1,-1,0)
		];

		perm = 0 ! 512;
		permMod12 = 0 ! 512;
		// To remove the need for index wrapping, double the permutation table length
		512.do{ arg i;
			perm[i] = p[i & 255];
			permMod12[i] = (perm[i] % 12);
		}
	}

	*new {
		Error("No need to instantiate Simplex...all methods are class methods").throw
	}

	// noise will be periodic at integer multiples of x (typically 0..1)
	*periodic { arg x, offset = #[31.4, -62.8], freqScale = 0.2, oct = 3;
		var phase = x * 2pi;
		var v = [cos(phase), sin(phase)] * freqScale + offset;
		^this.fBm2(v[0], v[1], oct)
	}

	*prfBm { arg vec, oct = 3, method;
		var sum = 0;
		var weight = 1.0;
		oct.do {
			sum = sum + (this.perform(method, *vec) * weight);
			vec = vec * 2;
			weight = weight / 2;
		}
		^sum
	}

	*fBm2 { arg xin, yin, oct = 3;
		^this.prfBm([xin, yin], oct, \noise2);
	}

	*fBm3 { arg xin, yin, zin, oct = 3;
		^this.prfBm([xin, yin, zin], oct, \noise3);
	}

	*fBm4 { arg xin, yin, zin, win, oct = 3;
		^this.prfBm([xin, yin, zin, win], oct, \noise4);
	}

	*noise2 { arg xin, yin;
		var n0, n1, n2; // Noise contributions from the three corners
		// Skew the input space to determine which simplex cell we're in
		var s = (xin+yin)*kF2; // Hairy factor for 2D
		var i = floor(xin+s).asInteger;
		var j = floor(yin+s).asInteger;
		var t = (i+j)*kG2;
		var uX0 = i-t; // Unskew the cell origin back to (x,y) space
		var uY0 = j-t;
		var x0 = xin-uX0; // The x,y distances from the cell origin
		var y0 = yin-uY0;

		// For the 2D case, the simplex shape is an equilateral triangle.
		// Determine which simplex we are in.
		var i1, j1; // Offsets for second (middle) corner of simplex in (i,j) coords
		var x1, y1, x2, y2;
		var ii, jj, gi0, gi1, gi2;
		var t0, t1, t2;

		if (x0>y0) {
			// lower triangle, XY order: (0,0)->(1,0)->(1,1)
			i1=1; j1=0;
		} {
			// upper triangle, YX order: (0,0)->(0,1)->(1,1)
			i1=0; j1=1;
		};
		// A step of (1,0) in (i,j) means a step of (1-c,-c) in (x,y), and
		// a step of (0,1) in (i,j) means a step of (-c,1-c) in (x,y), where
		// c = (3-sqrt(3))/6
		x1 = x0 - i1 + kG2; // Offsets for middle corner in (x,y) unskewed coords
		y1 = y0 - j1 + kG2;
		x2 = x0 - 1.0 + (2.0 * kG2); // Offsets for last corner in (x,y) unskewed coords
		y2 = y0 - 1.0 + (2.0 * kG2);

		// Work out the hashed gradient indices of the three simplex corners
		ii = i & 255;
		jj = j & 255;
		gi0 = permMod12[ii+perm[jj]];
		gi1 = permMod12[ii+i1+perm[jj+j1]];
		gi2 = permMod12[ii+1+perm[jj+1]];

		// Calculate the contribution from the three corners
		t0 = 0.5 - (x0*x0)-(y0*y0);
		if (t0<0) {
			n0 = 0.0;
		} {
			t0 = t0 * t0;
			n0 = t0 * t0 * grad3[gi0].dot2(x0, y0);  // (x,y) of grad3 used for 2D gradient
		};
		t1 = 0.5 - (x1*x1)-(y1*y1);
		if (t1<0) {
			n1 = 0.0;
		} {
			t1 = t1 * t1;
			n1 = t1 * t1 * grad3[gi1].dot2(x1, y1);
		};
		t2 = 0.5 - (x2*x2)-(y2*y2);
		if (t2<0) {
			n2 = 0.0;
		} {
			t2 = t2 * t2;
			n2 = t2 * t2 * grad3[gi2].dot2(x2, y2);
		};
		// Add contributions from each corner to get the final noise value.
		// The result is scaled to return values in the interval [-1,1].
		^70.0 * (n0 + n1 + n2);
	}

	*noise3 { arg xin, yin, zin;
		var n0, n1, n2, n3; // Noise contributions from the four corners
		// Skew the input space to determine which simplex cell we're in
		var s = (xin+yin+zin)*kF3; // Very nice and simple skew factor for 3D
		var i = floor(xin+s).asInteger;
		var j = floor(yin+s).asInteger;
		var k = floor(zin+s).asInteger;
		var t = (i+j+k)*kG3;
		var uX0 = i-t; // Unskew the cell origin back to (x,y,z) space
		var uY0 = j-t;
		var uZ0 = k-t;
		var x0 = xin-uX0; // The x,y,z distances from the cell origin
		var y0 = yin-uY0;
		var z0 = zin-uZ0;

		// For the 3D case, the simplex shape is a slightly irregular tetrahedron.
		// Determine which simplex we are in.
		var i1, j1, k1; // Offsets for second corner of simplex in (i,j,k) coords
		var i2, j2, k2; // Offsets for third corner of simplex in (i,j,k) coords
		var x1, y1, z1;
		var x2, y2, z2;
		var x3, y3, z3;
		var ii, jj, kk;
		var gi0, gi1, gi2, gi3;
		var t0, t1, t2, t3;

		if (x0>=y0) {
			if (y0>=z0) {
				// X Y Z order
				i1=1; j1=0; k1=0; i2=1; j2=1; k2=0;
			} {
				if (x0>=z0) {
					// X Z Y order
					i1=1; j1=0; k1=0; i2=1; j2=0; k2=1;
				} {
					// Z X Y order
					i1=0; j1=0; k1=1; i2=1; j2=0; k2=1;
				}
			}
		} {
			// x0<y0
			if (y0<z0) {
				// Z Y X order
				i1=0; j1=0; k1=1; i2=0; j2=1; k2=1;
			} {
				if(x0<z0) {
					// Y Z X order
					i1=0; j1=1; k1=0; i2=0; j2=1; k2=1;
				} {
					// Y X Z order
					i1=0; j1=1; k1=0; i2=1; j2=1; k2=0;
				}
			}
		};

		// A step of (1,0,0) in (i,j,k) means a step of (1-c,-c,-c) in (x,y,z),
		// a step of (0,1,0) in (i,j,k) means a step of (-c,1-c,-c) in (x,y,z), and
		// a step of (0,0,1) in (i,j,k) means a step of (-c,-c,1-c) in (x,y,z), where
		// c = 1/6.
		x1 = x0 - i1 + kG3; // Offsets for second corner in (x,y,z) coords
		y1 = y0 - j1 + kG3;
		z1 = z0 - k1 + kG3;
		x2 = x0 - i2 + (2.0*kG3); // Offsets for third corner in (x,y,z) coords
		y2 = y0 - j2 + (2.0*kG3);
		z2 = z0 - k2 + (2.0*kG3);
		x3 = x0 - 1.0 + (3.0*kG3); // Offsets for last corner in (x,y,z) coords
		y3 = y0 - 1.0 + (3.0*kG3);
		z3 = z0 - 1.0 + (3.0*kG3);

		// Work out the hashed gradient indices of the four simplex corners
		ii = i & 255;
		jj = j & 255;
		kk = k & 255;
		gi0 = permMod12[ii+perm[jj+perm[kk]]];
		gi1 = permMod12[ii+i1+perm[jj+j1+perm[kk+k1]]];
		gi2 = permMod12[ii+i2+perm[jj+j2+perm[kk+k2]]];
		gi3 = permMod12[ii+1+perm[jj+1+perm[kk+1]]];

		// Calculate the contribution from the four corners
		t0 = 0.6 - (x0*x0) - (y0*y0) - (z0*z0);
		if (t0<0) {
			n0 = 0.0;
		} {
			t0 = t0 * t0;
			n0 = t0 * t0 * grad3[gi0].dot3(x0, y0, z0);
		};
		t1 = 0.6 - (x1*x1) - (y1*y1) - (z1*z1);
		if (t1<0) {
			n1 = 0.0;
		} {
			t1 = t1 * t1;
			n1 = t1 * t1 * grad3[gi1].dot3(x1, y1, z1);
		};
		t2 = 0.6 - (x2*x2) - (y2*y2) - (z2*z2);
		if (t2<0) {
			n2 = 0.0;
		} {
			t2 = t2 * t2;
			n2 = t2 * t2 * grad3[gi2].dot3(x2, y2, z2);
		};
		t3 = 0.6 - (x3*x3) - (y3*y3) - (z3*z3);
		if (t3<0){
			n3 = 0.0;
		} {
			t3 = t3 * t3;
			n3 = t3 * t3 * grad3[gi3].dot3(x3, y3, z3);
		};
		// Add contributions from each corner to get the final noise value.
		// The result is scaled to stay just inside [-1,1]
		^32.0*(n0 + n1 + n2 + n3);
	}

	*noise4 { arg xin, yin, zin, win;
		var n0, n1, n2, n3, n4; // Noise contributions from the five corners
		// Skew the (x,y,z,w) space to determine which cell of 24 simplices we're in
		var s = (xin + yin + zin + win) * kF4; // Factor for 4D skewing
		var i = floor(xin + s).asInteger;
		var j = floor(yin + s).asInteger;
		var k = floor(zin + s).asInteger;
		var l = floor(win + s).asInteger;
		var t = (i + j + k + l) * kG4; // Factor for 4D unskewing
		var uX0 = i - t; // Unskew the cell origin back to (x,y,z,w) space
		var uY0 = j - t;
		var uZ0 = k - t;
		var uW0 = l - t;
		var x0 = xin - uX0;  // The x,y,z,w distances from the cell origin
		var y0 = yin - uY0;
		var z0 = zin - uZ0;
		var w0 = win - uW0;
		// For the 4D case, the simplex is a 4D shape I won't even try to describe.
		// To find out which of the 24 possible simplices we're in, we need to
		// determine the magnitude ordering of x0, y0, z0 and w0.
		// Six pair-wise comparisons are performed between each possible pair
		// of the four coordinates, and the results are used to rank the numbers.
		var rankx = 0;
		var ranky = 0;
		var rankz = 0;
		var rankw = 0;
		var i1, j1, k1, l1; // The integer offsets for the second simplex corner
		var i2, j2, k2, l2; // The integer offsets for the third simplex corner
		var i3, j3, k3, l3; // The integer offsets for the fourth simplex corner
		var x1, y1, z1, w1; // Offsets for second corner in (x,y,z,w) coords
		var x2, y2, z2, w2;  // Offsets for third corner in (x,y,z,w) coords
		var x3, y3, z3, w3;  // Offsets for fourth corner in (x,y,z,w) coords
		var x4, y4, z4, w4;  // Offsets for last corner in (x,y,z,w) coords
		var ii, jj, kk, ll;
		var gi0, gi1, gi2, gi3, gi4;
		var t0, t1, t2, t3, t4;

		if (x0 > y0) { rankx = rankx + 1 } { ranky = ranky + 1 };
		if (x0 > z0) { rankx = rankx + 1 } { rankz = rankz + 1 };
		if (x0 > w0) { rankx = rankx + 1 } { rankw = rankw + 1 };
		if (y0 > z0) { ranky = ranky + 1 } { rankz = rankz + 1 };
		if (y0 > w0) { ranky = ranky + 1 } { rankw = rankw + 1 };
		if (z0 > w0) { rankz = rankz + 1 } { rankw = rankw + 1 };
		// [rankx, ranky, rankz, rankw] is a 4-vector with the numbers 0, 1, 2 and 3
		// in some order. We use a thresholding to set the coordinates in turn.
		// Rank 3 denotes the largest coordinate.
		i1 = if (rankx >= 3) { 1 } { 0 };
		j1 = if (ranky >= 3) { 1 } { 0 };
		k1 = if (rankz >= 3) { 1 } { 0 };
		l1 = if (rankw >= 3) { 1 } { 0 };
		// Rank 2 denotes the second largest coordinate.
		i2 = if (rankx >= 2) { 1 } { 0 };
		j2 = if (ranky >= 2) { 1 } { 0 };
		k2 = if (rankz >= 2) { 1 } { 0 };
		l2 = if (rankw >= 2) { 1 } { 0 };
		// Rank 1 denotes the second smallest coordinate.
		i3 = if (rankx >= 1) { 1 } { 0 };
		j3 = if (ranky >= 1) { 1 } { 0 };
		k3 = if (rankz >= 1) { 1 } { 0 };
		l3 = if (rankw >= 1) { 1 } { 0 };
		// The fifth corner has all coordinate offsets = 1, so no need to compute that.
		x1 = x0 - i1 + kG4; // Offsets for second corner in (x,y,z,w) coords
		y1 = y0 - j1 + kG4;
		z1 = z0 - k1 + kG4;
		w1 = w0 - l1 + kG4;
		x2 = x0 - i2 + (2.0*kG4); // Offsets for third corner in (x,y,z,w) coords
		y2 = y0 - j2 + (2.0*kG4);
		z2 = z0 - k2 + (2.0*kG4);
		w2 = w0 - l2 + (2.0*kG4);
		x3 = x0 - i3 + (3.0*kG4); // Offsets for fourth corner in (x,y,z,w) coords
		y3 = y0 - j3 + (3.0*kG4);
		z3 = z0 - k3 + (3.0*kG4);
		w3 = w0 - l3 + (3.0*kG4);
		x4 = x0 - 1.0 + (4.0*kG4); // Offsets for last corner in (x,y,z,w) coords
		y4 = y0 - 1.0 + (4.0*kG4);
		z4 = z0 - 1.0 + (4.0*kG4);
		w4 = w0 - 1.0 + (4.0*kG4);

		// Work out the hashed gradient indices of the five simplex corners
		ii = i & 255;
		jj = j & 255;
		kk = k & 255;
		ll = l & 255;
		gi0 = perm[ii+perm[jj+perm[kk+perm[ll]]]] % 32;
		gi1 = perm[ii+i1+perm[jj+j1+perm[kk+k1+perm[ll+l1]]]] % 32;
		gi2 = perm[ii+i2+perm[jj+j2+perm[kk+k2+perm[ll+l2]]]] % 32;
		gi3 = perm[ii+i3+perm[jj+j3+perm[kk+k3+perm[ll+l3]]]] % 32;
		gi4 = perm[ii+1+perm[jj+1+perm[kk+1+perm[ll+1]]]] % 32;
		// Calculate the contribution from the five corners
		t0 = 0.6 - (x0*x0) - (y0*y0) - (z0*z0) - (w0*w0);
		if (t0<0) {
			n0 = 0.0;
		} {
			t0 = t0 * t0;
			n0 = t0 * t0 * grad4[gi0].dot4(x0, y0, z0, w0);
		};
		t1 = 0.6 - (x1*x1) - (y1*y1) - (z1*z1) - (w1*w1);
		if (t1<0) {
			n1 = 0.0;
		} {
			t1 = t1 * t1;
			n1 = t1 * t1 * grad4[gi1].dot4(x1, y1, z1, w1);
		};
		t2 = 0.6 - (x2*x2) - (y2*y2) - (z2*z2) - (w2*w2);
		if (t2<0) {
			n2 = 0.0;
		} {
			t2 = t2 * t2;
			n2 = t2 * t2 * grad4[gi2].dot4(x2, y2, z2, w2);
		};
		t3 = 0.6 - (x3*x3) - (y3*y3) - (z3*z3) - (w3*w3);
		if (t3<0) {
			n3 = 0.0;
		} {
			t3 = t3 * t3;
			n3 = t3 * t3 * grad4[gi3].dot4(x3, y3, z3, w3);
		};
		t4 = 0.6 - (x4*x4) - (y4*y4) - (z4*z4) - (w4*w4);
		if (t4<0) {
			n4 = 0.0;
		} {
			t4 = t4 * t4;
			n4 = t4 * t4 * grad4[gi4].dot4(x4, y4, z4, w4);
		};
		// Sum up and scale the result to cover the range [-1,1]
		^27.0 * (n0 + n1 + n2 + n3 + n4);
	}

}

SimplexGrad {
	var x, y, z, w;

	*new { arg x, y, z, w = 0;
		^super.newCopyArgs(x, y, z, w);
	}

	dot2 { arg xin, yin;
		^(x * xin) + (y * yin)
	}

	dot3 { arg xin, yin, zin;
		^(x * xin) + (y * yin) + (z * zin)
	}

	dot4 { arg xin, yin, zin, win;
		^(x * xin) + (y * yin) + (z * zin) + (w * win)
	}
}
