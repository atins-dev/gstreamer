// This file was generated by the Gtk# code generator.
// Any changes made will be lost if regenerated.

namespace Gst.Sdp {

	using System;
	using System.Collections;
	using System.Collections.Generic;
	using System.Runtime.InteropServices;

#region Autogenerated code
	[StructLayout(LayoutKind.Sequential)]
	public partial struct MIKEYPayloadPKE : IEquatable<MIKEYPayloadPKE> {

		private IntPtr _pt;
		public Gst.Sdp.MIKEYPayload Pt {
			get {
				return _pt == IntPtr.Zero ? null : (Gst.Sdp.MIKEYPayload) GLib.Opaque.GetOpaque (_pt, typeof (Gst.Sdp.MIKEYPayload), false);
			}
			set {
				_pt = value == null ? IntPtr.Zero : value.Handle;
			}
		}
		public Gst.Sdp.MIKEYCacheType C;
		public ushort DataLen;
		private IntPtr _data;

		public static Gst.Sdp.MIKEYPayloadPKE Zero = new Gst.Sdp.MIKEYPayloadPKE ();

		public static Gst.Sdp.MIKEYPayloadPKE New(IntPtr raw) {
			if (raw == IntPtr.Zero)
				return Gst.Sdp.MIKEYPayloadPKE.Zero;
			return (Gst.Sdp.MIKEYPayloadPKE) Marshal.PtrToStructure (raw, typeof (Gst.Sdp.MIKEYPayloadPKE));
		}

		public bool Equals (MIKEYPayloadPKE other)
		{
			return true && Pt.Equals (other.Pt) && C.Equals (other.C) && DataLen.Equals (other.DataLen) && _data.Equals (other._data);
		}

		public override bool Equals (object other)
		{
			return other is MIKEYPayloadPKE && Equals ((MIKEYPayloadPKE) other);
		}

		public override int GetHashCode ()
		{
			return this.GetType ().FullName.GetHashCode () ^ Pt.GetHashCode () ^ C.GetHashCode () ^ DataLen.GetHashCode () ^ _data.GetHashCode ();
		}

		private static GLib.GType GType {
			get { return GLib.GType.Pointer; }
		}
#endregion
	}
}